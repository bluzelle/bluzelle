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

namespace bzn
{

    class pbft_newview_test : public pbft_proto_test
    {
    public:

        std::shared_ptr<Mockcrypto_base>
        build_pft_with_mock_crypto()
        {
            std::shared_ptr<Mockcrypto_base> mockcrypto = std::make_shared<Mockcrypto_base>();
            this->crypto = mockcrypto;
            this->build_pbft();
            return mockcrypto;
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

        void
        run_transaction_through_primary_times(const size_t repeat, uint64_t& current_sequence)
        {
            for (size_t i{0}; i<repeat; ++i)
            {
                current_sequence++;
                run_transaction_through_primary(false);
            }
        }

        size_t
        max_faulty_replicas_allowed() { return TEST_PEER_LIST.size() / 3; }

//        void
//        execute_handle_failure_expect_sut_to_send_viewchange()
//        {
//            // I expect that a replica forced to handle a failure will invalidate
//            // its' view, and cause the replica to send a VIEWCHANGE messsage
//            EXPECT_CALL(*mock_node, send_message_str(_, _))
//                    .WillRepeatedly(Invoke([&](const auto & /*endpoint*/, const auto encoded_message) {
//                        bzn_envelope envelope;
//                        envelope.ParseFromString(*encoded_message);
//
//                        pbft_msg view_change;
//                        view_change.ParseFromString(envelope.pbft());
//                        EXPECT_EQ(PBFT_MSG_VIEWCHANGE, view_change.type());
//                        EXPECT_TRUE(2 == view_change.view());
//                        EXPECT_TRUE(this->pbft->latest_stable_checkpoint().first == view_change.sequence());
//                    }));
//            this->pbft->handle_failure();
//        }
//
//        void
//        send_some_viewchange_messages(size_t n, bool(*f)(std::shared_ptr<std::string> wrapped_msg))
//        {
//            size_t count{0};
//            EXPECT_CALL(*mock_node, send_message_str(_, ResultOf(f, Eq(true))))
//                    .Times(Exactly(TEST_PEER_LIST.size()))
//                    .WillRepeatedly(Invoke([&](auto &, auto &) {
//
//
//                        EXPECT_EQ( n, count);
//
//
//
//
//                    }));
//
//            pbft_msg pbft_msg;
//            pbft_msg.set_type(PBFT_MSG_VIEWCHANGE);
//            pbft_msg.set_view(this->pbft->get_view() + 1);
//
//            // let's pretend that the sytem under test is receiving view change messages
//            // from the other replicas
//            for (const auto &peer : TEST_PEER_LIST)
//            {
//                if (peer.uuid == this->uuid)
//                {
//                    continue;
//                }
//
//                count++;
//                this->pbft->handle_message(pbft_msg, this->default_original_msg);
//                if (count == n)
//                {
//                    break;
//                }
//            }
//        }
//
//        void
//        set_checkpoint(uint64_t sequence)
//        {
//            for (const auto &peer : TEST_PEER_LIST)
//            {
//                pbft_msg msg;
//                msg.set_type(PBFT_MSG_CHECKPOINT);
//                msg.set_view(this->pbft->get_view());
//                msg.set_sequence(sequence);
//                msg.set_state_hash(std::to_string(sequence));
//
//                wrapped_bzn_msg wmsg;
//                wmsg.set_type(BZN_MSG_PBFT);
//                wmsg.set_sender(peer.uuid);
//                // wmsg.set_signature(???)
//                wmsg.set_payload(msg.SerializeAsString());
//
//                this->pbft->handle_message(msg, this->default_original_msg);
//            }
//        }
    };

    TEST_F(pbft_newview_test, test_pre_prepares_contiguous)
    {
        auto set_pre_prepare_sequence = [](pbft_msg& sut, const uint64_t sequence)
        {
            bzn_envelope envelope;
            pbft_msg pre_prepare;
            pre_prepare.set_sequence(sequence);
            envelope.set_pbft(pre_prepare.SerializeAsString());
            *(sut.add_pre_prepare_messages()) = envelope;
        };

        pbft_msg sut;

        // empty pre_prepare list is contiguous
        EXPECT_TRUE(pbft::pre_prepares_contiguous(sut));

        // add two contiguous pre preps
        set_pre_prepare_sequence(sut, 837465);
        set_pre_prepare_sequence(sut, 837466);
        EXPECT_TRUE(pbft::pre_prepares_contiguous(sut));

        // missed pre preps must fail
        set_pre_prepare_sequence(sut, 837468);
        EXPECT_FALSE(pbft::pre_prepares_contiguous(sut));

        sut.clear_pre_prepare_messages();

        // out of order pre prepares must fail
        set_pre_prepare_sequence(sut, 837466);
        set_pre_prepare_sequence(sut, 837465);
        EXPECT_FALSE(pbft::pre_prepares_contiguous(sut));

        sut.clear_pre_prepare_messages();

        // duplicate pre prepare sequences must fail
        set_pre_prepare_sequence(sut, 837465);
        set_pre_prepare_sequence(sut, 837466);
        set_pre_prepare_sequence(sut, 837466);
        set_pre_prepare_sequence(sut, 837467);
        EXPECT_FALSE(pbft::pre_prepares_contiguous(sut));

        sut.clear_pre_prepare_messages();

        // lets make a big list
        for(uint64_t i{450}; i<550; ++i)
        {
          set_pre_prepare_sequence(sut, i);
        }
        EXPECT_TRUE(pbft::pre_prepares_contiguous(sut));
    }

    TEST_F(pbft_newview_test, make_newview)
    {
        uint64_t current_sequence{0};
        this->generate_checkpoint_at_sequence_100(current_sequence);

        this->run_transaction_through_primary_times(2, current_sequence);

        bzn_envelope viewchange_envelope;
        EXPECT_CALL(*mock_node, send_message(_, ResultOf(test::is_viewchange, Eq(true))))
                .WillRepeatedly(Invoke([&](const auto & /*endpoint*/, const auto& viewchange_env) {viewchange_envelope = *viewchange_env;}));
        this->pbft->handle_failure();

        pbft_msg viewchange;

        viewchange.ParseFromString(viewchange_envelope.pbft());

        uint64_t                            new_view_index{viewchange.view()};
        std::map<uuid_t,bzn_envelope>       viewchange_envelopes_from_senders;
        std::map<uint64_t, bzn_envelope>    pre_prepare_messages;


        // we can generate valid newview now

        pbft_msg newview{this->pbft->make_newview(new_view_index, viewchange_envelopes_from_senders, pre_prepare_messages)};

        EXPECT_EQ(PBFT_MSG_NEWVIEW, newview.type());
        EXPECT_EQ(new_view_index, newview.view());

        //EXPECT_TRUE(newview.viewchange_messages_size() + 1 <= newview.viewchange_messages_size());




        // TODO view_change_messages, pre_prepare_messages
        // ...
    }
//
//    TEST_F(pbft_newview_test, build_newview)
//    {
//        const uint64_t          new_view_index = 34867;
//        std::vector<pbft_msg>   viewchange_messages;
//
//        this->build_pbft();
//
//        pbft_msg newview{this->pbft->build_newview(
//                new_view_index
//                , viewchange_messages)};
//
//        EXPECT_EQ(PBFT_MSG_NEWVIEW, newview.type());
//        EXPECT_EQ(new_view_index, newview.view());
//
//        // TODO viewchange_messages
//    }
//
//    // bool is_valid_newview_message(const pbft_msg& msg) const;
//    TEST_F(pbft_newview_test, is_valid_newview_message)
//    {
//        this->build_pbft();
//        pbft_msg newview;
//        bzn_envelope original_msg;
//
//        EXPECT_FALSE(this->pbft->is_valid_newview_message(newview, original_msg));
//    }
//
//    // void handle_newview(const pbft_msg& msg, const bzn_envelope& original_msg);
//    TEST_F(pbft_newview_test, primary_handle_newview)
//    {
//        this->build_pbft();
//
//        pbft_msg msg;
//        bzn_envelope original_msg;
//
//        this->pbft->handle_newview(msg, original_msg);
//
//    }
//
//    // void handle_newview(const pbft_msg& msg, const bzn_envelope& original_msg);
//    TEST_F(pbft_newview_test, backup_handle_newview)
//    {
//        this->uuid = SECOND_NODE_UUID;
//        this->build_pbft();
//
//        pbft_msg msg;
//        bzn_envelope original_msg;
//
//        this->pbft->handle_newview(msg, original_msg);
//    }
//
////    TEST_F(pbft_newview_test, validate_and_extract_checkpoint_hashes)
////    {
////        this->build_pbft();
////
////        pbft_msg viewchange_message;
////
////        // there should be no checkpoints
////        EXPECT_EQ( (size_t)0 , this->pbft->validate_and_extract_checkpoint_hashes(viewchange_message).size());
////
////        // add an empty envelope
////        bzn_envelope envelope;
////
////
////        // there should be no checkpoints
////        EXPECT_EQ( (size_t)0 , this->pbft->validate_and_extract_checkpoint_hashes(viewchange_message).size());
////
////        viewchange_message.clear_checkpoint_messages();
////
////
////        // generate checkpoint messages and add them to the envelope
////        //const uint64_t new_view_index = 48593;
////
////        for (const auto& peer : TEST_PEER_LIST)
////        {
////            envelope.set_sender(peer.uuid);
////            envelope.set_signature("valid_signature");
////
////            pbft_msg checkpoint_message;
////            checkpoint_message.set_type(PBFT_MSG_CHECKPOINT);
////            envelope.set_pbft(checkpoint_message.SerializeAsString());
////        }
////    }

    TEST_F(pbft_newview_test, test_get_primary)
    {
        build_pbft();

        // the pbft sut must be the current view's primay
        EXPECT_EQ(this->uuid, this->pbft->get_primary().uuid);

        this->pbft->view++;
        EXPECT_FALSE(this->uuid == this->pbft->get_primary().uuid);

        // given a view, get_primary must provide the address of a primary

        // TODO: this is a pretty sketchy test.
        for (size_t view{0}; view < 100; ++view)
        {
            const bzn::uuid_t uuid = this->pbft->get_primary(view).uuid;
            const bzn::uuid_t accepted_uuid = this->pbft->current_peers()[view % this->pbft->current_peers().size()].uuid;
            EXPECT_EQ(uuid, accepted_uuid);
        }
    }

//    TEST_F(pbft_newview_test, is_valid_view)
//    {
//        this->build_pbft();
//        // initially the view should be valid
//        EXPECT_TRUE(this->pbft->is_view_valid());
//
//        // after a failure the view should be invalid
//        this->pbft->handle_failure();
//        EXPECT_FALSE(this->pbft->is_view_valid());
//    }
//
//
////    TEST_F(pbft_newview_test, get_sequences_and_request_hashes_from_proofs)
////    {
////        uint64_t current_sequence{0};
////        generate_checkpoint_at_sequence_100(current_sequence);
////
////        current_sequence++;
////        run_transaction_through_primary(false);
////        current_sequence++;
////        run_transaction_through_primary(false);
////
////        // let pbft generate a view change message for us
////
////        bzn_envelope viewchange_envelope;
////        EXPECT_CALL(*mock_node, send_message(_, ResultOf(test::is_viewchange, Eq(true))))
////                .WillRepeatedly(Invoke(
////                        [&](const auto & /*endpoint*/, const auto& viewchange_env)
////                        {
////                            viewchange_envelope = *viewchange_env;
////                        }));
////
////        this->pbft->handle_failure();
////
////        pbft_msg viewchange;
////        EXPECT_TRUE(viewchange.ParseFromString(viewchange_envelope.pbft()));
////
////        std::set<std::pair<uint64_t, std::string>> sequence_request_pairs;
////        EXPECT_TRUE(this->pbft->get_sequences_and_request_hashes_from_proofs(viewchange, sequence_request_pairs));
////
////
////        LOG(info) << sequence_request_pairs.size();
////    }


}