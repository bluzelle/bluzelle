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

#include <mocks/mock_boost_asio_beast.hpp>
#include <mocks/mock_node_base.hpp>
#include <mocks/mock_session_base.hpp>
#include <mocks/mock_crypto_base.hpp>
#include <pbft/test/pbft_proto_test.hpp>

// https://coveralls.io/builds/20296851/source?filename=pbft/pbft.cpp


namespace bzn
{
    class pbft_viewchange_test : public pbft_proto_test
    {
    public:
        std::shared_ptr<bzn::options_base> options = std::make_shared<bzn::options>();

        std::shared_ptr<Mockcrypto_base>
        build_pft_with_mock_crypto()
        {
            std::shared_ptr<Mockcrypto_base> mockcrypto = std::make_shared<Mockcrypto_base>();
            this->crypto = mockcrypto;
            this->build_pbft();
            return mockcrypto;
        }

        void
        run_transaction_through_primary_times(const size_t repeat, uint64_t& current_sequence)
        {
            for (size_t i{0}; i<repeat; ++i)
            {
                current_sequence++;
                run_transaction_through_primary(false);
            }
        }

        void
        generate_checkpoint_at_sequence_100(uint64_t& current_sequence)
        {
            auto mockcrypto = this->build_pft_with_mock_crypto();

            EXPECT_CALL(*mockcrypto, hash(An<const bzn_envelope&>()))
                    .WillRepeatedly(Invoke([&](const bzn_envelope& envelope)
                                           {
                                               return envelope.sender() + "_" + std::to_string(current_sequence) + "_" + std::to_string(envelope.timestamp());
                                           }));

            EXPECT_CALL(*mockcrypto, verify(_))
                    .WillRepeatedly(Invoke([&](const bzn_envelope& /*msg*/)
                                           {
                                               return true;
                                           }));

            for (current_sequence=1; current_sequence < 100; ++current_sequence)
            {
                run_transaction_through_primary();
            }
            prepare_for_checkpoint(current_sequence);
            run_transaction_through_primary();
            this->stabilize_checkpoint(current_sequence);
        }
    };

    TEST_F(pbft_viewchange_test, test_fill_in_missing_pre_prepares)
    {
        std::map<uint64_t, bzn_envelope> pre_prepares;

        auto mock_crypto = build_pft_with_mock_crypto();

        //std::pair<uint64_t, bzn::hash_t>;
        this->pbft->stable_checkpoint = std::make_pair(uint64_t(100), "<checkpoint hash value>");

        EXPECT_CALL(*mock_crypto, sign(_)).WillOnce(Invoke([&](bzn_envelope& msg)
                                                          {
                                                              msg.set_sender(this->pbft->get_uuid());
                                                              msg.set_signature("mock_signature");
                                                              return true;
                                                          }));
        bzn_envelope envelope;

        pre_prepares.insert(std::make_pair(uint64_t(this->pbft->stable_checkpoint.first+3), envelope));
        pre_prepares.insert(std::make_pair(uint64_t(this->pbft->stable_checkpoint.first+1), envelope));
        this->pbft->fill_in_missing_pre_prepares(4, pre_prepares);

        EXPECT_TRUE(0 < pre_prepares.count(this->pbft->stable_checkpoint.first+2));


        EXPECT_CALL(*mock_crypto, sign(_)).WillRepeatedly(Invoke([&](bzn_envelope& msg)
                                                           {
                                                               msg.set_sender(this->pbft->get_uuid());
                                                               msg.set_signature("mock_signature");
                                                               return true;
                                                           }));

        pre_prepares.insert(std::make_pair(uint64_t(this->pbft->stable_checkpoint.first+7), envelope));

        this->pbft->fill_in_missing_pre_prepares(4, pre_prepares);

        uint64_t sequence = this->pbft->stable_checkpoint.first+1;
        for(const auto pre_prepare : pre_prepares)
        {
            EXPECT_TRUE(0 < pre_prepares.count(sequence));
            ++sequence;
        }

        EXPECT_TRUE(0 == pre_prepares.count(sequence));
    }

    TEST_F(pbft_viewchange_test, pbft_with_invalid_view_drops_messages)
    {
        this->uuid = SECOND_NODE_UUID;
        this->build_pbft();

        this->pbft->handle_failure();

        // after handling the failure, the pbft must ignore all messages save for
        // checkpoint, view change and new view messages
        pbft_msg message;

        message.set_type(PBFT_MSG_PREPREPARE);
        EXPECT_FALSE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_PREPARE);
        EXPECT_FALSE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_COMMIT);
        EXPECT_FALSE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_CHECKPOINT);
        EXPECT_TRUE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_JOIN);
        EXPECT_FALSE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_LEAVE);
        EXPECT_FALSE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_VIEWCHANGE);
        EXPECT_TRUE(this->pbft->preliminary_filter_msg(message));

        message.set_type(PBFT_MSG_NEWVIEW);
        EXPECT_TRUE(this->pbft->preliminary_filter_msg(message));
    }


    TEST_F(pbft_viewchange_test, test_make_signed_envelope)
    {
        const std::string mock_signature{"signature"};

        auto mockcrypto = this->build_pft_with_mock_crypto();

        pbft_msg message;
        message.set_type(PBFT_MSG_VIEWCHANGE);
        message.set_sequence(383439);
        message.set_request_hash("request_hash");
        message.set_view(484575);

        EXPECT_CALL(*mockcrypto, sign(_)).WillOnce(Invoke([&](bzn_envelope& msg)
        {
            msg.set_sender(this->pbft->get_uuid());
            msg.set_signature(mock_signature);
            return true;
        }));
        bzn_envelope signed_envelope = this->pbft->make_signed_envelope(message.SerializeAsString());

        EXPECT_EQ(TEST_NODE_UUID, signed_envelope.sender());
        EXPECT_EQ(mock_signature, signed_envelope.signature());
    }

    TEST_F(pbft_viewchange_test, test_is_peer)
    {
        const bzn::peer_address_t NOT_PEER{"127.0.0.1", 9091, 9991, "not_a_peer", "uuid_nope"};
        this->build_pbft();

        for (const auto& peer : TEST_PEER_LIST)
        {
            EXPECT_TRUE(this->pbft->is_peer(peer.uuid));
        }

        EXPECT_FALSE(this->pbft->is_peer(NOT_PEER.uuid));
    }

    TEST_F(pbft_viewchange_test, validate_and_extract_checkpoint_hashes)
    {
        uint64_t current_sequence{0};
        generate_checkpoint_at_sequence_100(current_sequence);

        this->run_transaction_through_primary_times(2, current_sequence);

        EXPECT_CALL(*mock_node, send_message(_, ResultOf(test::is_viewchange, Eq(true))))
                .WillRepeatedly(Invoke(
                        [&](const auto & /*endpoint*/, const auto& viewchange_env)
                        {
                            pbft_msg viewchange;
                            viewchange.ParseFromString(viewchange_env->pbft());
                            EXPECT_EQ(PBFT_MSG_VIEWCHANGE, viewchange.type());

                            std::map<bzn::checkpoint_t , std::set<bzn::uuid_t>> checkpoints = this->pbft->validate_and_extract_checkpoint_hashes(viewchange);
                            EXPECT_EQ(uint64_t(1), checkpoints.size());

                            for (const auto& p : checkpoints)
                            {
                                auto checkpoint = p.first;
                                auto uuids = p.second;

                                // there will be a checkpoint 100, with a hashe value of "100"
                                EXPECT_EQ( uint64_t(100), checkpoint.first );
                                EXPECT_EQ( "100", checkpoint.second );

                                EXPECT_EQ( uint64_t(3), uuids.size());
                                for (const auto& uuid : uuids)
                                {
                                    EXPECT_FALSE(TEST_PEER_LIST.end() == std::find_if(TEST_PEER_LIST.begin(), TEST_PEER_LIST.end(), [&](const auto& peer)
                                    {
                                        return peer.uuid == uuid;
                                    }));
                                }
                            }
                        }));
        this->pbft->handle_failure();
    }

    TEST_F(pbft_viewchange_test, validate_viewchange_checkpoints)
    {
        uint64_t current_sequence{0};

        this->generate_checkpoint_at_sequence_100(current_sequence);

        this->run_transaction_through_primary_times(2, current_sequence);

        EXPECT_CALL(*mock_node, send_message(_, ResultOf(test::is_viewchange, Eq(true))))
                .WillRepeatedly(Invoke([&](const auto & /*endpoint*/, const auto viewchange_env)
                {
                    pbft_msg viewchange;

                    EXPECT_EQ(this->pbft->get_uuid(), viewchange_env->sender());
                    EXPECT_TRUE(viewchange.ParseFromString(viewchange_env->pbft()));

                    auto pair = this->pbft->validate_viewchange_checkpoints(viewchange);
                    uint64_t checkpoint = pair->first;
                    hash_t hash = pair->second;

                    EXPECT_EQ(uint64_t(100), checkpoint);
                    LOG (debug) << hash;
                }));
        this->pbft->handle_failure();
    }

    TEST_F(pbft_viewchange_test, DISABLED_is_valid_viewchange_message)
    {
        uint64_t current_sequence{0};

        generate_checkpoint_at_sequence_100(current_sequence);

        this->run_transaction_through_primary_times(2, current_sequence);


        EXPECT_CALL(*mock_node, send_message(_, ResultOf(test::is_viewchange, Eq(true))))
                .WillRepeatedly(Invoke([&](const auto & /*endpoint*/, const auto viewchange_env)
                {
                    EXPECT_EQ(this->pbft->get_uuid(), viewchange_env->sender());
                    pbft_msg viewchange;
                    EXPECT_TRUE(viewchange.ParseFromString(viewchange_env->pbft())); // this will be valid.
                    EXPECT_TRUE(this->pbft->is_valid_viewchange_message(viewchange, *viewchange_env));
                }));

        this->pbft->handle_failure();
    }

    TEST_F(pbft_viewchange_test, make_viewchange_makes_valid_message)
    {
        uint64_t current_sequence{0};

        generate_checkpoint_at_sequence_100(current_sequence);

        this->run_transaction_through_primary_times(2, current_sequence);

        auto viewchange = this->pbft->make_viewchange(this->pbft->get_view() + uint64_t(1), current_sequence, this->pbft->stable_checkpoint_proof, this->pbft->prepared_operations_since_last_checkpoint());

        EXPECT_EQ(PBFT_MSG_VIEWCHANGE, viewchange.type());
        EXPECT_EQ(current_sequence, viewchange.sequence());
        EXPECT_EQ(3, viewchange.checkpoint_messages_size());
    }

    TEST_F(pbft_viewchange_test, pbft_handle_failure_causes_invalid_view_state_and_starts_viewchange)
    {
        this->uuid = SECOND_NODE_UUID;
        this->build_pbft();

        EXPECT_CALL(*mock_node, send_message_str(_, _))
                .WillRepeatedly(Invoke([&](const auto & /*endpoint*/, const auto encoded_message)
                                       {
                                           bzn_envelope envelope;
                                           envelope.ParseFromString(*encoded_message);


                                           pbft_msg view_change;
                                           view_change.ParseFromString(envelope.pbft());
                                           EXPECT_EQ(PBFT_MSG_VIEWCHANGE, view_change.type());
                                           EXPECT_TRUE(2 == view_change.view());
                                           EXPECT_TRUE(this->pbft->latest_stable_checkpoint().first == view_change.sequence());
                                       }));

        this->pbft->handle_failure();

        // Now the replica's view should be invalid
        EXPECT_FALSE(this->pbft->is_view_valid());
    }

    TEST_F(pbft_viewchange_test, test_prepared_operations_since_last_checkpoint)
    {
        uint64_t current_sequence{0};

        generate_checkpoint_at_sequence_100(current_sequence);

        EXPECT_EQ(size_t(0), this->pbft->prepared_operations_since_last_checkpoint().size());

        run_transaction_through_primary(false);
        current_sequence++;
        EXPECT_EQ(size_t(1), this->pbft->prepared_operations_since_last_checkpoint().size());

        run_transaction_through_primary(false);
        current_sequence++;
        EXPECT_EQ(size_t(2), this->pbft->prepared_operations_since_last_checkpoint().size());

        auto operations = this->pbft->prepared_operations_since_last_checkpoint();
        for(const auto& operation : operations)
        {
            // TODO: what other tests?
            EXPECT_EQ(uint64_t(1), operation->view);
            EXPECT_TRUE(operation->sequence > 100 && operation->sequence <= current_sequence);
        }
    }
}