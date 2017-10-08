/*
 * Copyright (c) 2017, Uber Technologies, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "uber/jaeger/samplers/RemotelyControlledSampler.h"

#include <cassert>
#include <sstream>

#include "uber/jaeger/metrics/Counter.h"
#include "uber/jaeger/metrics/Gauge.h"
#include "uber/jaeger/samplers/AdaptiveSampler.h"
#include "uber/jaeger/utils/Net.h"

namespace uber {
namespace jaeger {
namespace samplers {
namespace {

bool isUnreserved(char ch)
{
    if (std::isalpha(ch) || std::isdigit(ch)) {
        return true;
    }

    switch (ch) {
    case '-':
    case '.':
    case '_':
    case '~':
        return true;
    default:
        return false;
    }
}

std::string percentEncode(const std::string& input)
{
    std::ostringstream oss;
    for (auto&& ch : input) {
        if (isUnreserved(ch)) {
            oss << ch;
        }
        else {
            oss << '%' << std::uppercase << std::hex << static_cast<int>(ch);
        }
    }
    return oss.str();
}

class HTTPSamplingManager : public thrift::sampling_manager::SamplingManagerIf {
  public:
    using SamplingStrategyResponse
        = thrift::sampling_manager::SamplingStrategyResponse;

    explicit HTTPSamplingManager(const std::string& serverURL)
        : _serverURI(utils::net::URI::parse(serverURL))
        , _serverAddr()
    {
        auto addressInfo =
            utils::net::resolveAddress(_serverURI._host, AF_INET);
        for (auto itr = addressInfo.get(); itr; itr = itr->ai_next) {
            try {
                utils::net::Socket socket;
                socket.open(AF_INET);
                socket.connect(*reinterpret_cast<::sockaddr_in*>(itr->ai_addr));
                socket.close();
                std::memcpy(&_serverAddr,
                            itr->ai_addr,
                            itr->ai_addrlen);
                break;
            } catch (...) {
                if (!itr->ai_next) {
                    throw;
                }
            }
        }
    }

    void getSamplingStrategy(SamplingStrategyResponse& result,
                             const std::string& serviceName) override
    {
        const auto target =
            _serverURI._path + "?service=" + percentEncode(serviceName);
        std::ostringstream oss;
        oss << "GET " << target << " HTTP/1.1\r\n"
            << "Host: " << _serverURI._host << "\r\n"
            // TODO: << "User-Agent: jaeger/" << kJaegerClientVersion
            << "\r\n\r\n";
        utils::net::Socket socket;
        socket.open(AF_INET);
        socket.connect(*reinterpret_cast<::sockaddr_in*>(&_serverAddr));;
        const auto request = oss.str();
        ::write(socket.handle(), request.c_str(), request.size());
        constexpr auto kBufferSize = 256;
        std::array<char, kBufferSize> buffer;
        std::string response;
        for (auto numRead = 0;
             numRead > 0;
             numRead = ::read(socket.handle(), &buffer[0], kBufferSize)) {
            response += std::string(&buffer[0], &buffer[numRead]);
        }
        std::cout << response << '\n';
        /* TODO:
        const auto response = httpGetRequest(uri);
        const auto jsonValue = nlohmann::json::parse(response);
        result = jsonValue.get<SamplingStrategyResponse>();*/
    }

  private:
    utils::net::URI _serverURI;
    ::sockaddr _serverAddr;
};

}  // anonymous namespace

RemotelyControlledSampler::RemotelyControlledSampler(
    const std::string& serviceName, const SamplerOptions& options)
    : _options(options)
    , _serviceName(serviceName)
    , _manager(
          std::make_shared<HTTPSamplingManager>(_options.samplingServerURL()))
    , _running(true)
    , _mutex()
    , _shutdownCV()
    , _thread([this]() { pollController(); })
{
}

SamplingStatus
RemotelyControlledSampler::isSampled(const TraceID& id,
                                     const std::string& operation)
{
    std::lock_guard<std::mutex> lock(_mutex);
    assert(_options.sampler());
    return _options.sampler()->isSampled(id, operation);
}

void RemotelyControlledSampler::close()
{
    std::unique_lock<std::mutex> lock(_mutex);
    _running = false;
    lock.unlock();
    _shutdownCV.notify_one();
    _thread.join();
}

void RemotelyControlledSampler::pollController()
{
    while (_running) {
        updateSampler();
        std::unique_lock<std::mutex> lock(_mutex);
        _shutdownCV.wait_for(lock,
                             _options.samplingRefreshInterval(),
                             [this]() { return !_running; });
    }
}

void RemotelyControlledSampler::updateSampler()
{
    assert(_manager);
    assert(_options.metrics());
    thrift::sampling_manager::SamplingStrategyResponse response;
    try {
        assert(_manager);
        _manager->getSamplingStrategy(response, _serviceName);
    } catch (const std::exception& ex) {
        _options.metrics()->samplerQueryFailure().inc(1);
        return;
    } catch (...) {
        _options.metrics()->samplerQueryFailure().inc(1);
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _options.metrics()->samplerRetrieved().inc(1);

    if (response.__isset.operationSampling) {
        updateAdaptiveSampler(response.operationSampling);
    }
    else {
        try {
            updateRateLimitingOrProbabilisticSampler(response);
        } catch (const std::exception& ex) {
            _options.metrics()->samplerUpdateFailure().inc(1);
            return;
        } catch (...) {
            _options.metrics()->samplerUpdateFailure().inc(1);
            return;
        }
    }
    _options.metrics()->samplerUpdated().inc(1);
}

void RemotelyControlledSampler::updateAdaptiveSampler(
    const PerOperationSamplingStrategies& strategies)
{
    auto sampler = _options.sampler();
    assert(sampler);
    if (sampler->type() == Type::kAdaptiveSampler) {
        static_cast<AdaptiveSampler&>(*sampler).update(strategies);
    }
    else {
        sampler = std::make_shared<AdaptiveSampler>(strategies,
                                                    _options.maxOperations());
        _options.setSampler(sampler);
    }
}

void RemotelyControlledSampler::updateRateLimitingOrProbabilisticSampler(
    const SamplingStrategyResponse& response)
{
    std::shared_ptr<Sampler> sampler;
    if (response.__isset.probabilisticSampling) {
        sampler = std::make_shared<ProbabilisticSampler>(
            response.probabilisticSampling.samplingRate);
    }
    else if (response.__isset.rateLimitingSampling) {
        sampler = std::make_shared<RateLimitingSampler>(
            response.rateLimitingSampling.maxTracesPerSecond);
    }
    else {
        std::ostringstream oss;
        oss << "Unsupported sampling strategy type " << response.strategyType;
        throw std::runtime_error(oss.str());
    }
    _options.setSampler(sampler);
}

}  // namespace samplers
}  // namespace jaeger
}  // namespace uber
