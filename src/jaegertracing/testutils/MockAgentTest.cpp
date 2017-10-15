/*
 * Copyright (c) 2017 Uber Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <thrift/protocol/TJSONProtocol.h>

#include "jaegertracing/net/http/Response.h"
#include "jaegertracing/testutils/MockAgent.h"

namespace jaegertracing {
namespace testutils {

TEST(MockAgent, testSpanServer)
{
    std::shared_ptr<MockAgent> mockAgent = MockAgent::make();
    mockAgent->start();

    auto client = mockAgent->spanServerClient();

    constexpr auto kBiggestBatch = 5;
    for (auto i = 1; i < kBiggestBatch; ++i) {
        thrift::Batch batch;
        batch.spans.resize(i);
        for (auto j = 0; j < i; ++j) {
            std::string operationName("span-");
            operationName += std::to_string(j);
            batch.spans[j].__set_operationName(operationName);
        }

        client->emitBatch(batch);

        constexpr auto kNumTries = 100;
        for (auto k = 0; k < kNumTries; ++k) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto batches = mockAgent->batches();
            if (!batches.empty() &&
                static_cast<int>(batches[0].spans.size()) == i) {
                break;
            }
        }

        const auto batches = mockAgent->batches();
        ASSERT_FALSE(batches.empty());
        ASSERT_EQ(i, static_cast<int>(batches[0].spans.size()));
        for (auto j = 0; j < i; ++j) {
            std::string operationName("span-");
            operationName += std::to_string(j);
            ASSERT_EQ(operationName, batches[0].spans[j].operationName);
        }
        mockAgent->resetBatches();
    }
}

TEST(MockAgent, testSamplingManager)
{
    auto mockAgent = MockAgent::make();
    mockAgent->start();

    {
        std::ostringstream oss;
        oss << "http://" << mockAgent->samplingServerAddr().authority() << '/';
        const auto uriStr = oss.str();
        const auto uri = net::URI::parse(uriStr);
        const auto response = net::http::get(uri);
        ASSERT_EQ("no 'service' parameter", response.body());
    }
    {
        std::ostringstream oss;
        oss << "http://" << mockAgent->samplingServerAddr().authority()
            << "/?service=a&service=b";
        const auto uriStr = oss.str();
        const auto uri = net::URI::parse(uriStr);
        const auto response = net::http::get(uri);
        ASSERT_EQ("'service' parameter must occur only once", response.body());
    }
    {
        std::ostringstream oss;
        oss << "http://" << mockAgent->samplingServerAddr().authority()
            << "/?service=something";
        const auto uriStr = oss.str();
        const auto uri = net::URI::parse(uriStr);
        const auto responseHTTP = net::http::get(uri);
        thrift::sampling_manager::SamplingStrategyResponse response;
        boost::shared_ptr<apache::thrift::transport::TMemoryBuffer> transport(
            new apache::thrift::transport::TMemoryBuffer());
        apache::thrift::protocol::TJSONProtocol protocol(transport);
        std::vector<uint8_t> buffer;
        buffer.reserve(responseHTTP.body().size());
        buffer.insert(std::end(buffer),
                      std::begin(responseHTTP.body()),
                      std::end(responseHTTP.body()));
        transport->write(&buffer[0], buffer.size());
        response.read(&protocol);
        ASSERT_EQ(thrift::sampling_manager::SamplingStrategyType::PROBABILISTIC,
                  response.strategyType);
    }
    {
        thrift::sampling_manager::SamplingStrategyResponse config;
        config.__set_strategyType(
            thrift::sampling_manager::SamplingStrategyType::RATE_LIMITING);
        thrift::sampling_manager::RateLimitingSamplingStrategy rateLimiting;
        rateLimiting.__set_maxTracesPerSecond(123);
        config.__set_rateLimitingSampling(rateLimiting);
        mockAgent->addSamplingStrategy("service123", config);

        std::ostringstream oss;
        oss << "http://" << mockAgent->samplingServerAddr().authority()
            << "/?service=service123";
        const auto uriStr = oss.str();
        const auto uri = net::URI::parse(uriStr);
        const auto responseHTTP = net::http::get(uri);
        thrift::sampling_manager::SamplingStrategyResponse response;
        boost::shared_ptr<apache::thrift::transport::TMemoryBuffer> transport(
            new apache::thrift::transport::TMemoryBuffer());
        apache::thrift::protocol::TJSONProtocol protocol(transport);
        std::vector<uint8_t> buffer;
        buffer.reserve(responseHTTP.body().size());
        buffer.insert(std::end(buffer),
                      std::begin(responseHTTP.body()),
                      std::end(responseHTTP.body()));
        transport->write(&buffer[0], buffer.size());
        response.read(&protocol);
        ASSERT_EQ(config, response);
    }
}

}  // namespace testutils
}  // namespace jaegertracing
