// Copyright (C) 2018 Bluzelle
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License, version 3,
// as published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include <gtest/gtest.h>
#include <storage/mem_storage.hpp>
#include <proto/pbft.pb.h>
#include <proto/database.pb.h>
#include <pbft/operations/pbft_persistent_operation.hpp>
#include <pbft/operations/pbft_operation.hpp>
#include <boost/range/irange.hpp>
#include <mocks/smart_mock_peers_beacon.hpp>

using namespace ::testing;

namespace
{
    const std::vector<bzn::uuid_t> UUIDS{"alice", "bob", "cindy", "dave"};
    const std::vector<bzn::peer_address_t> TEST_PEER_LIST{{  "127.0.0.1", 8081, "name1", "alice"}
                                                          , {"127.0.0.1", 8082, "name2", "bob"}
                                                          , {"127.0.0.1", 8083, "name3", "cindy"}
                                                          , {"127.0.0.1", 8084, "name4", "dave"}};

    void record_pbft_messages(int from, int until, pbft_msg_type type, std::shared_ptr<bzn::pbft_operation> op)
    {
        pbft_msg message;
        message.set_view(op->get_view());
        message.set_sequence(op->get_sequence());
        message.set_request_hash(op->get_request_hash());
        message.set_type(type);

        for (; from<until; from++)
        {
            bzn_envelope message_env;

            message_env.set_pbft(message.SerializeAsString());
            message_env.set_sender(UUIDS.at(from));

            op->record_pbft_msg(message, message_env);
        }
    }

    void record_request(std::shared_ptr<bzn::pbft_operation> op, uint64_t nonce = 6)
    {
        database_msg request;
        bzn_envelope request_env;

        request.mutable_header()->set_nonce(nonce);

        request_env.set_database_msg(request.SerializeAsString());
        request_env.set_sender("a client");

        op->record_request(request_env);
    }

    class persistent_operation_test : public Test
    {
    public:

        const uint64_t view = 1;
        const uint64_t sequence = 2;
        const std::string request_hash = "a very hashy hash";
        const size_t peers_size = 4;

        std::shared_ptr<bzn::peers_beacon_base> static_beacon = static_peers_beacon_for(TEST_PEER_LIST);

        std::shared_ptr<bzn::storage_base> storage = std::make_shared<bzn::mem_storage>();
        std::shared_ptr<bzn::pbft_operation> operation = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);

    };

    TEST_F(persistent_operation_test, remembers_state_after_rehydrate)
    {
        record_request(this->operation);
        record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, this->operation);
        record_pbft_messages(0, 4, PBFT_MSG_PREPARE, this->operation);
        this->operation->advance_operation_stage(bzn::pbft_operation_stage::commit, static_peers_beacon_for(TEST_PEER_LIST));

        EXPECT_TRUE(this->operation->is_ready_for_commit(this->static_beacon));
        EXPECT_EQ(this->operation->get_stage(), bzn::pbft_operation_stage::commit);

        this->operation = nullptr;
        auto op2 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);
        EXPECT_TRUE(op2->is_ready_for_commit(this->static_beacon));
        EXPECT_EQ(op2->get_stage(), bzn::pbft_operation_stage::commit);

        auto op3 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence+1, this->request_hash, this->storage);
        EXPECT_FALSE(op3->is_ready_for_commit(this->static_beacon));
        EXPECT_EQ(op3->get_stage(), bzn::pbft_operation_stage::prepare);
    }

    TEST_F(persistent_operation_test, remembers_request_after_rehydrate)
    {
        record_request(this->operation, 9999u);
        EXPECT_TRUE(this->operation->has_db_request());
        EXPECT_EQ(this->operation->get_database_msg().header().nonce(), 9999u);

        this->operation = nullptr;
        auto op2 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);
        EXPECT_TRUE(op2->has_db_request());
        EXPECT_EQ(op2->get_database_msg().header().nonce(), 9999u);

        auto op3 = std::make_shared<bzn::pbft_persistent_operation>(this->view+1, this->sequence, this->request_hash, this->storage);
        EXPECT_FALSE(op3->has_db_request());
    }

    TEST_F(persistent_operation_test, continue_progressing_state_after_rehydrate)
    {
        record_request(this->operation);
        record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, this->operation);
        record_pbft_messages(0, 2, PBFT_MSG_PREPARE, this->operation);

        EXPECT_EQ(this->operation->get_stage(), bzn::pbft_operation_stage::prepare);
        EXPECT_TRUE(this->operation->is_preprepared());
        EXPECT_TRUE(this->operation->has_request());

        this->operation = nullptr;
        auto op2 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);

        EXPECT_EQ(op2->get_stage(), bzn::pbft_operation_stage::prepare);
        EXPECT_TRUE(op2->is_preprepared());
        EXPECT_TRUE(op2->has_request());

        record_pbft_messages(2, 4, PBFT_MSG_PREPARE, op2);
        EXPECT_TRUE(op2->is_ready_for_commit(this->static_beacon));
        op2->advance_operation_stage(bzn::pbft_operation_stage::commit, this->static_beacon);

        record_pbft_messages(0, 4, PBFT_MSG_COMMIT, op2);
        EXPECT_TRUE(op2->is_ready_for_execute(this->static_beacon));
        op2->advance_operation_stage(bzn::pbft_operation_stage::execute, this->static_beacon);
    }

    TEST_F(persistent_operation_test, no_contamination_from_different_request)
    {
        auto op2 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);
        auto op3 = std::make_shared<bzn::pbft_persistent_operation>(this->view+1, this->sequence, this->request_hash, this->storage);
        auto op4 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash+"xx", this->storage);

        //op2 gets just a preprepare, op3 gets 2f prepares, op4 gets 2f+1 prepares

        for (const auto& op : std::vector<std::shared_ptr<bzn::pbft_persistent_operation>>{op2, op3, op4})
        {
            record_request(op);
            record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, op);
        }

        record_pbft_messages(0, 2, PBFT_MSG_PREPARE, op3);
        record_pbft_messages(0, 3, PBFT_MSG_PREPARE, op4);

        op4->advance_operation_stage(bzn::pbft_operation_stage::commit, this->static_beacon);

        EXPECT_FALSE(op2->is_ready_for_commit(this->static_beacon));
        EXPECT_FALSE(op3->is_ready_for_commit(this->static_beacon));
        EXPECT_TRUE(op4->is_ready_for_commit(this->static_beacon));
    }

    TEST_F(persistent_operation_test, remembers_messages_after_rehydrate)
    {
        record_request(this->operation);
        record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, this->operation);
        record_pbft_messages(0, 2, PBFT_MSG_PREPARE, this->operation);

        this->operation = nullptr;
        auto op2 = std::make_shared<bzn::pbft_persistent_operation>(this->view, this->sequence, this->request_hash, this->storage);

        record_pbft_messages(2, 4, PBFT_MSG_PREPARE, op2);
        op2->advance_operation_stage(bzn::pbft_operation_stage::commit, this->static_beacon);

        EXPECT_TRUE(op2->is_ready_for_commit(this->static_beacon));
        EXPECT_EQ(op2->get_preprepare().sender(), UUIDS.at(0));
        EXPECT_EQ(op2->get_prepares().size(), 4u);
    }

    TEST_F(persistent_operation_test, test_prepared_in_range)
    {
        for (auto i : boost::irange(0, 100))
        {
            auto op = std::make_shared<bzn::pbft_persistent_operation>(1, i, "some_hash", this->storage);
            record_request(op);
            record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, op);

            // record 1-4 prepares
            record_pbft_messages(0, (i % 4) + 1, PBFT_MSG_PREPARE, op);
            if ((i % 4 + 1) > 2)
            {
                op->advance_operation_stage(bzn::pbft_operation_stage::commit, this->static_beacon);
            }
        }

        EXPECT_EQ(bzn::pbft_persistent_operation::prepared_operations_in_range(this->storage, 0, 100).size(), 50u);
    }

    TEST_F(persistent_operation_test, test_remove_range)
    {
        for (auto i : boost::irange(0, 100))
        {
            auto op = std::make_shared<bzn::pbft_persistent_operation>(1, i, "some_hash", this->storage);
            record_request(op);
            record_pbft_messages(0, 1, PBFT_MSG_PREPREPARE, op);
        }

        // note - there's an extra operation in there from the constructor
        EXPECT_EQ(this->storage->get_size(bzn::pbft_persistent_operation::get_uuid()).first, 301u);
        bzn::pbft_persistent_operation::remove_range(this->storage, 50, 60);
        EXPECT_EQ(this->storage->get_size(bzn::pbft_persistent_operation::get_uuid()).first, 271u);

        bzn::pbft_persistent_operation::remove_range(this->storage, 0, 10);
        EXPECT_EQ(this->storage->get_size(bzn::pbft_persistent_operation::get_uuid()).first, 240u);
    }
}
