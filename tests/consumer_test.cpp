#include <vector>
#include <thread>
#include <set>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include "cppkafka/consumer.h"
#include "cppkafka/producer.h"

using std::vector;
using std::move;
using std::string;
using std::thread;
using std::set;
using std::mutex;
using std::condition_variable;
using std::lock_guard;
using std::unique_lock;
using std::chrono::seconds;
using std::chrono::milliseconds;
using std::chrono::system_clock;

using namespace cppkafka;

class ConsumerRunner {
public:
    ConsumerRunner(Consumer& consumer, size_t expected, size_t partitions) 
    : consumer_(consumer) {
        bool booted = false;
        mutex mtx;
        condition_variable cond;
        thread_ = thread([&, expected, partitions]() {
            consumer_.set_timeout(milliseconds(500));
            size_t number_eofs = 0;
            auto start = system_clock::now();
            while (system_clock::now() - start < seconds(10) && messages_.size() < expected) {
                Message msg = consumer_.poll();
                if (msg && number_eofs != partitions && msg.get_error() == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
                    number_eofs++;
                    if (number_eofs == partitions) {
                        lock_guard<mutex> _(mtx);
                        booted = true;
                        cond.notify_one();
                    }
                }
                else if (msg && msg.get_error() == 0) {
                    messages_.push_back(move(msg));
                }
            }
        });

        unique_lock<mutex> lock(mtx);
        while (!booted) {
            cond.wait(lock);
        }
    }



    ConsumerRunner(const ConsumerRunner&) = delete;
    ConsumerRunner& operator=(const ConsumerRunner&) = delete;

    ~ConsumerRunner() {
        try_join();
    }

    const std::vector<Message>& get_messages() const {
        return messages_;
    }

    void try_join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }
private:
    Consumer& consumer_;
    thread thread_;
    std::vector<Message> messages_;
};

class ConsumerTest : public testing::Test {
public:
    static const string KAFKA_TOPIC;

    Configuration make_producer_config() {
        Configuration config;
        config.set("metadata.broker.list", KAFKA_TEST_INSTANCE);
        return config;
    }

    Configuration make_consumer_config() {
        Configuration config;
        config.set("metadata.broker.list", KAFKA_TEST_INSTANCE);
        config.set("enable.auto.commit", "false");
        config.set("group.id", "consumer_test");
        return config;
    }
};

const string ConsumerTest::KAFKA_TOPIC = "cppkafka_test1";

TEST_F(ConsumerTest, AssignmentCallback) {
    vector<TopicPartition> assignment;
    int partition = 0;

    // Create a consumer and subscribe to the topic
    Consumer consumer(make_consumer_config());
    consumer.set_assignment_callback([&](const vector<TopicPartition>& topic_partitions) {
        assignment = topic_partitions;
    });
    consumer.subscribe({ KAFKA_TOPIC });
    ConsumerRunner runner(consumer, 1, 3);

    // Produce a message just so we stop the consumer
    Producer producer(make_producer_config());
    Topic topic = producer.get_topic(KAFKA_TOPIC);
    string payload = "Hello world!";
    producer.produce(topic, partition, Buffer(payload.data(), payload.size()));
    runner.try_join();

    // All 3 partitions should be ours
    EXPECT_EQ(3, assignment.size());
    set<int> partitions = { 0, 1, 2 }; 
    for (const auto& topic_partition : assignment) {
        EXPECT_EQ(KAFKA_TOPIC, topic_partition.get_topic());
        EXPECT_TRUE(partitions.erase(topic_partition.get_partition()));
    }
    EXPECT_EQ(1, runner.get_messages().size());

    assignment = consumer.get_assignment();
    EXPECT_EQ(3, assignment.size());
}

TEST_F(ConsumerTest, Rebalance) {
    vector<TopicPartition> assignment1;
    vector<TopicPartition> assignment2;
    bool revocation_called = false;
    int partition = 0;

    // Create a consumer and subscribe to the topic
    Consumer consumer1(make_consumer_config());
    consumer1.set_assignment_callback([&](const vector<TopicPartition>& topic_partitions) {
        assignment1 = topic_partitions;
    });
    consumer1.set_revocation_callback([&](const vector<TopicPartition>&) {
        revocation_called = true;
    });
    consumer1.subscribe({ KAFKA_TOPIC });
    ConsumerRunner runner1(consumer1, 1, 3);

    // Create a second consumer and subscribe to the topic
    Consumer consumer2(make_consumer_config());
    consumer2.set_assignment_callback([&](const vector<TopicPartition>& topic_partitions) {
        assignment2 = topic_partitions;
    });
    consumer2.subscribe({ KAFKA_TOPIC });
    ConsumerRunner runner2(consumer2, 1, 1);

    EXPECT_TRUE(revocation_called);

    // Produce a message just so we stop the consumer
    Producer producer(make_producer_config());
    Topic topic = producer.get_topic(KAFKA_TOPIC);
    string payload = "Hello world!";
    producer.produce(topic, partition, Buffer(payload.data(), payload.size()));
    runner1.try_join();
    runner2.try_join();

    // All 3 partitions should be assigned
    EXPECT_EQ(3, assignment1.size() + assignment2.size());
    set<int> partitions = { 0, 1, 2 }; 
    for (const auto& topic_partition : assignment1) {
        EXPECT_EQ(KAFKA_TOPIC, topic_partition.get_topic());
        EXPECT_TRUE(partitions.erase(topic_partition.get_partition()));
    }
    for (const auto& topic_partition : assignment2) {
        EXPECT_EQ(KAFKA_TOPIC, topic_partition.get_topic());
        EXPECT_TRUE(partitions.erase(topic_partition.get_partition()));
    }
    EXPECT_EQ(1, runner1.get_messages().size() + runner2.get_messages().size());
}
