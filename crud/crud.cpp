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

#include <crud/crud.hpp>
#include <policy/random.hpp>
#include <policy/volatile_ttl.hpp>
#include <utils/make_endpoint.hpp>
#include <functional>
#include <boost/algorithm/string/trim_all.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/random/uniform_int_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>

using namespace bzn;
using namespace std::placeholders;


namespace
{
    const std::string PERMISSION_UUID{"PERMS"};
    const std::string OWNER_KEY{"OWNER"};
    const std::string WRITERS_KEY{"WRITERS"};
    const std::string MAX_SIZE_KEY{"MAX_SIZE"};
    const std::string EVICTION_POLICY_KEY{"EVICTION_POLICY"};

    const std::string TTL_UUID{"TTL"};
    const std::chrono::seconds TTL_TICK{5}; // not too aggressive

    inline bzn::key_t generate_expire_key(const bzn::uuid_t& uuid, const bzn::key_t& key)
    {
        Json::Value value;

        value["uuid"] = uuid;
        value["key"] = key;

        return value.toStyledString();
    }

    inline std::pair<bzn::uuid_t, bzn::key_t> extract_uuid_key(const bzn::key_t& key)
    {
        Json::CharReaderBuilder rbuilder;
        std::unique_ptr<Json::CharReader> const reader(rbuilder.newCharReader());
        std::string parse_errors;

        Json::Value json;

        if (!reader->parse(key.c_str(), key.c_str() + key.size(), &json, &parse_errors))
        {
            throw std::runtime_error("Failed to parse database json ttl data: " + parse_errors);
        }

        return std::make_pair(json["uuid"].asString(), json["key"].asString());
    }
}


crud::crud(std::shared_ptr<bzn::asio::io_context_base> io_context,
           std::shared_ptr<bzn::storage_base> storage,
           std::shared_ptr<bzn::subscription_manager_base> subscription_manager,
           std::shared_ptr<bzn::node_base> node, bzn::key_t owner_public_key)
           : storage(std::move(storage))
           , subscription_manager(std::move(subscription_manager))
           , node(std::move(node))
           , expire_timer(io_context->make_unique_steady_timer())
           , message_handlers{
                 {database_msg::kCreate,        std::bind(&crud::handle_create,         this, _1, _2, _3)},
                 {database_msg::kRead,          std::bind(&crud::handle_read,           this, _1, _2, _3)},
                 {database_msg::kUpdate,        std::bind(&crud::handle_update,         this, _1, _2, _3)},
                 {database_msg::kDelete,        std::bind(&crud::handle_delete,         this, _1, _2, _3)},
                 {database_msg::kHas,           std::bind(&crud::handle_has,            this, _1, _2, _3)},
                 {database_msg::kKeys,          std::bind(&crud::handle_keys,           this, _1, _2, _3)},
                 {database_msg::kSize,          std::bind(&crud::handle_size,           this, _1, _2, _3)},
                 {database_msg::kSubscribe,     std::bind(&crud::handle_subscribe,      this, _1, _2, _3)},
                 {database_msg::kUnsubscribe,   std::bind(&crud::handle_unsubscribe,    this, _1, _2, _3)},
                 {database_msg::kCreateDb,      std::bind(&crud::handle_create_db,      this, _1, _2, _3)},
                 {database_msg::kUpdateDb,      std::bind(&crud::handle_update_db,      this, _1, _2, _3)},
                 {database_msg::kDeleteDb,      std::bind(&crud::handle_delete_db,      this, _1, _2, _3)},
                 {database_msg::kHasDb,         std::bind(&crud::handle_has_db,         this, _1, _2, _3)},
                 {database_msg::kWriters,       std::bind(&crud::handle_writers,        this, _1, _2, _3)},
                 {database_msg::kAddWriters,    std::bind(&crud::handle_add_writers,    this, _1, _2, _3)},
                 {database_msg::kRemoveWriters, std::bind(&crud::handle_remove_writers, this, _1, _2, _3)},
                 {database_msg::kQuickRead,     std::bind(&crud::handle_read,           this, _1, _2, _3)},
                 {database_msg::kTtl,           std::bind(&crud::handle_ttl,            this, _1, _2, _3)},
                 {database_msg::kPersist,       std::bind(&crud::handle_persist,        this, _1, _2, _3)},
                 {database_msg::kExpire,        std::bind(&crud::handle_expire,         this, _1, _2, _3)}}
           , owner_public_key(std::move(owner_public_key))
{
}


void
crud::start(std::shared_ptr<bzn::pbft_base> pbft, const size_t max_swarm_storage)
{
    std::call_once(this->start_once,
        [this, pbft, max_swarm_storage]()
        {
            this->pbft = std::move(pbft);

            this->max_swarm_storage = max_swarm_storage;

            this->subscription_manager->start();

            this->expire_timer->expires_from_now(TTL_TICK);
            this->expire_timer->async_wait(std::bind(&crud::check_key_expiration, shared_from_this(), std::placeholders::_1));
        });
}


void
crud::handle_request(const bzn::caller_id_t& caller_id, const database_msg& request, const std::shared_ptr<bzn::session_base> session)
{
    if (auto it = this->message_handlers.find(request.msg_case()); it != this->message_handlers.end())
    {
        LOG(debug) << "processing message: " << uint32_t(request.msg_case());

        it->second(caller_id, request, session);

        return;
    }

    LOG(error) << "unknown request: " << uint32_t(request.msg_case());
}


void
crud::send_response(const database_msg& request, const bzn::storage_result result,
    database_response&& response, std::shared_ptr<bzn::session_base>& session)
{
    *response.mutable_header() = request.header();

    if (result != bzn::storage_result::ok)
    {
        if (bzn::storage_result_msg.count(result))
        {
			// special response error case...
			if (request.msg_case() == database_msg::kQuickRead)
			{
				response.mutable_quick_read()->set_error(bzn::storage_result_msg.at(result));
			}
        	else
			{
				response.mutable_error()->set_message(bzn::storage_result_msg.at(result));
			}
        }
        else
        {
            LOG(error) << "unknown error code: " << uint32_t(result);
        }
    }

    bzn_envelope env;
    env.set_database_response(response.SerializeAsString());

    if (session)
    {
        // special response case that does not require signing...
        if (request.msg_case() == database_msg::kQuickRead)
        {
            session->send_message(std::make_shared<bzn::encoded_message >(env.SerializeAsString()));
        }
        else
        {
            LOG(trace) << "Sending response via session";
            session->send_signed_message(std::make_shared<bzn_envelope>(env));
        }
    }
    else
    {
        LOG(warning) << "session not set - response for the " << uint32_t(request.msg_case()) << " operation not sent via session";
    }

    if (this->node && !response.header().point_of_contact().empty())
    {
        LOG(trace) << "Sending response via PoC: " << response.header().point_of_contact();
        try
        {
            this->node->send_signed_message(response.header().point_of_contact(), std::make_shared<bzn_envelope>(env));
        }
        catch(const std::runtime_error& err)
        {
            LOG(error) << err.what();
        }
    }
    else
    {
        LOG(warning) << "Unable to send response for the " << uint32_t(request.msg_case()) << " operation to point of contact - node not set in crud module";
    }
}


void
crud::handle_create(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_a_writer(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            // bail on key value pairs that are too large right away!
            if (this->max_database_size(perms) && request.create().key().length() + request.create().value().length() > this->max_database_size(perms))
            {
                this->send_response(request, bzn::storage_result::value_too_large, database_response(), session);

                return;
            }

            if (this->expired(request.header().db_uuid(), request.create().key()))
            {
                this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

                return;
            }

            if (this->operation_exceeds_available_space(request, perms))
            {
                if (!this->do_eviction(request, this->max_database_size(perms)))
                {
                    this->send_response(request, bzn::storage_result::db_full, database_response(), session);

                    return;
                }
            }

            result = this->storage->create(request.header().db_uuid(), request.create().key(), request.create().value());

            if (result == bzn::storage_result::ok)
            {
                this->update_expiration_entry(generate_expire_key(request.header().db_uuid(), request.create().key()),
                        request.create().expire());

                this->subscription_manager->inspect_commit(request);
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_read(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    if (!this->storage->has(PERMISSION_UUID, request.header().db_uuid()))
    {
        this->send_response(request, bzn::storage_result::db_not_found, database_response(), session);

        return;
    }

    const bzn::key_t key = (request.msg_case() == database_msg::kRead) ? request.read().key() : request.quick_read().key();

    // expired?
    if (this->expired(request.header().db_uuid(), key))
    {
        this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

        return;
    }

    const auto result = this->storage->read(request.header().db_uuid(), key);

    database_response response;

    if (result)
    {
        if (request.msg_case() == database_msg::kRead)
        {
            response.mutable_read()->set_key(key);
            response.mutable_read()->set_value(*result);
        }
        else
        {
            response.mutable_quick_read()->set_key(key);
            response.mutable_quick_read()->set_value(*result);
        }
    }

    this->send_response(request, (result) ? bzn::storage_result::ok : bzn::storage_result::not_found, std::move(response), session);
}


void
crud::handle_update(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_a_writer(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            // bail on key value pairs that are too large right away!
            if (this->max_database_size(perms) && request.create().key().length() + request.create().value().length() > this->max_database_size(perms))
            {
                this->send_response(request, bzn::storage_result::value_too_large, database_response(), session);

                return;
            }

            // expired?
            if (this->expired(request.header().db_uuid(), request.update().key()))
            {
                this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

                return;
            }

            if (this->operation_exceeds_available_space(request, perms))
            {
                // let's try evicting some key/value pairs
                if (!this->do_eviction(request, this->max_database_size(perms)))
                {
                    this->send_response(request, bzn::storage_result::db_full, database_response(), session);

                    return;
                }
            }

            result = this->storage->update(request.header().db_uuid(), request.update().key(), request.update().value());

            if (result == bzn::storage_result::ok)
            {
                this->update_expiration_entry(generate_expire_key(request.header().db_uuid(), request.update().key()), request.update().expire());

                this->subscription_manager->inspect_commit(request);
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_delete(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_a_writer(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            result = this->storage->remove(request.header().db_uuid(), request.delete_().key());

            if (result == bzn::storage_result::ok)
            {
                this->subscription_manager->inspect_commit(request);

                this->remove_expiration_entry(generate_expire_key(request.header().db_uuid(), request.delete_().key()));
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_ttl(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    bool has = this->storage->has(request.header().db_uuid(), request.ttl().key());

    // exists and expired?
    if (has && this->expired(request.header().db_uuid(), request.ttl().key()))
    {
        this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

        return;
    }

    database_response response;

    if (has)
    {
        const auto ttl = this->get_ttl(request.header().db_uuid(), request.ttl().key());

        if (ttl)
        {
            response.mutable_ttl()->set_key(request.ttl().key());
            response.mutable_ttl()->set_ttl(*ttl);
        }
        else
        {
            has = false; // we don't have a ttl value for this key
        }
    }

    this->send_response(request, (has) ? bzn::storage_result::ok : bzn::storage_result::ttl_not_found, std::move(response), session);
}


void
crud::handle_persist(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_a_writer(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            const auto generated_key = generate_expire_key(request.header().db_uuid(), request.persist().key());

            const bool has = this->storage->has(TTL_UUID, generated_key);

            // expired?
            if (has && this->expired(request.header().db_uuid(), request.persist().key()))
            {
                this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

                return;
            }

            if (has)
            {
                this->remove_expiration_entry(generated_key);

                result = bzn::storage_result::ok;
            }
            else
            {
                result = bzn::storage_result::ttl_not_found;
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_expire(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_a_writer(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            const auto generated_key = generate_expire_key(request.header().db_uuid(), request.expire().key());

            const bool has = this->storage->has(TTL_UUID, generated_key);

            // expired?
            if (has && this->expired(request.header().db_uuid(), request.expire().key()))
            {
                this->send_response(request, bzn::storage_result::delete_pending, database_response(), session);

                return;
            }

            // do not allow zero expires...
            if (request.expire().expire() == 0)
            {
                result = bzn::storage_result::invalid_argument;
            }
            else
            {
                result = bzn::storage_result::not_found;

                // assume if ttl entry exists so does the db entry...
                if (has)
                {
                    this->remove_expiration_entry(generated_key);

                    result = bzn::storage_result::ok;

                    this->update_expiration_entry(generated_key, request.expire().expire());
                }
                else
                {
                    if (this->storage->has(request.header().db_uuid(), request.expire().key()))
                    {
                        result = bzn::storage_result::ok;

                        this->update_expiration_entry(generated_key, request.expire().expire());
                    }
                    else
                    {
                        result = bzn::storage_result::not_found;
                    }
                }
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_has(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::ok};

    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    database_response response;

    response.mutable_has()->set_key(request.has().key());

    if (this->expired(request.header().db_uuid(), request.has().key()))
    {
        response.mutable_has()->set_has(false);
    }
    else
    {
        if (this->storage->has(PERMISSION_UUID, request.header().db_uuid()))
        {
            response.mutable_has()->set_has(this->storage->has(request.header().db_uuid(), request.has().key()));
        }
        else
        {
            result = bzn::storage_result::db_not_found;
        }
    }

    this->send_response(request, result, std::move(response), session);
}


void
crud::handle_keys(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::ok};

    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    database_response response;

    if (this->storage->has(PERMISSION_UUID, request.header().db_uuid()))
    {
        const auto keys = this->storage->get_keys(request.header().db_uuid());

        response.mutable_keys();

        for (const auto& key : keys)
        {
            if (!this->expired(request.header().db_uuid(), key))
            {
                response.mutable_keys()->add_keys(key);
            }
        }
    }
    else
    {
        result = bzn::storage_result::db_not_found;
    }

    this->send_response(request, result, std::move(response), session);
}


void
crud::handle_size(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (!db_exists)
    {
        this->send_response(request, bzn::storage_result::db_not_found, database_response(), session);

        return;
    }

    const auto [keys, size] = this->storage->get_size(request.header().db_uuid());

    database_response response;

    response.mutable_size()->set_keys(keys);
    response.mutable_size()->set_bytes(size);

    if (const auto max_size = this->max_database_size(perms); max_size)
    {
        response.mutable_size()->set_remaining_bytes((size < max_size) ? (max_size - size) : (0));
        response.mutable_size()->set_max_size(max_size);
    }

    this->send_response(request, bzn::storage_result::ok, std::move(response), session);
}


void
crud::handle_subscribe(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    if (session)
    {
        database_response response;

        this->subscription_manager->subscribe(request.header().db_uuid(), request.subscribe().key(),
            request.header().nonce(), response, session);

        this->send_response(request, bzn::storage_result::ok, std::move(response), session);

        return;
    }

    LOG(warning) << "session no longer available. SUBSCRIBE not executed.";
}


void
crud::handle_unsubscribe(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    if (session)
    {
        database_response response;

        this->subscription_manager->unsubscribe(request.header().db_uuid(), request.unsubscribe().key(),
            request.unsubscribe().nonce(), response, session);

        this->send_response(request, bzn::storage_result::ok, std::move(response), session);

        return;
    }

    // subscription manager will cleanup stale sessions...
    LOG(warning) << "session no longer available. UNSUBSCRIBE not executed.";
}


void
crud::handle_create_db(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::ok};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    if (!this->owner_public_key.empty() && (this->owner_public_key != caller_id))
    {
        result = bzn::storage_result::access_denied;
    }
    else if (this->storage->has(PERMISSION_UUID, request.header().db_uuid()))
    {
        result = bzn::storage_result::db_exists;
    }
    else
    {
        const Json::Value perms = this->create_permission_data(caller_id, request.create_db());

        // Check max_database_size and if requested database is set to unlimited!
        if ((request.create_db().max_size() == 0) && this->max_swarm_storage)
        {
            LOG(debug) << "attempting to create a database with no limits (max_swarm_storage = " << this->max_swarm_storage << ")";

            result = bzn::storage_result::invalid_size;
        }
        else
        {
            if (!this->operation_exceeds_available_space(request, perms))
            {
                result = this->storage->create(PERMISSION_UUID, request.header().db_uuid(), perms.toStyledString());
            }
            else
            {
                result = bzn::storage_result::db_full;
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_update_db(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_owner(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            // Check max_database_size and if requested database is set to unlimited!
            if ((request.update_db().max_size() == 0) && this->max_swarm_storage)
            {
                LOG(debug) << "attempting to update a database with no limits (max_swarm_storage = " << this->max_swarm_storage << ")";

                result = bzn::storage_result::invalid_size;
            }
            else
            {
                // only check if max size has grown...
                if (request.update_db().max_size() > perms[MAX_SIZE_KEY].asUInt64())
                {
                    auto new_perms = perms;

                    new_perms[MAX_SIZE_KEY] = request.update_db().max_size();

                    if (this->operation_exceeds_available_space(request, new_perms))
                    {
                        this->send_response(request, bzn::storage_result::db_full, database_response(), session);

                        return;
                    }
                }

                result = this->storage->update(PERMISSION_UUID, request.header().db_uuid(),
                    this->update_permission_data(perms, request.update_db()));
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_delete_db(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (!this->owner_public_key.empty() && (this->owner_public_key != caller_id))
    {
        result = bzn::storage_result::access_denied;
    }
    else if (db_exists)
    {
        if (!this->is_caller_owner(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            result = this->storage->remove(PERMISSION_UUID, request.header().db_uuid());

            this->storage->remove(request.header().db_uuid());

            this->flush_expiration_entries(request.header().db_uuid());
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_has_db(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    database_response response;

    response.mutable_has_db()->set_uuid(request.header().db_uuid());
    response.mutable_has_db()->set_has(this->storage->has(PERMISSION_UUID, request.header().db_uuid()));

    this->send_response(request, bzn::storage_result::ok, std::move(response), session);
}


void
crud::handle_writers(const bzn::caller_id_t& /*caller_id*/, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
     bzn::storage_result result{bzn::storage_result::not_found};
     std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

     const auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

     if (db_exists)
     {
         database_response resp;

         resp.mutable_writers()->set_owner(perms[OWNER_KEY].asString());

         for(const auto& writer : perms[WRITERS_KEY])
         {
             resp.mutable_writers()->add_writers(writer.asString());
         }

         this->send_response(request, bzn::storage_result::ok, std::move(resp), session);

         return;
     }
     else
     {
         this->send_response(request, result, database_response(), session);
     }
}


void
crud::handle_add_writers(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_owner(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            this->add_writers(request, perms);

            LOG(debug) << "updating db perms: " << perms.toStyledString().substr(0, MAX_MESSAGE_SIZE) << "...";

            if (result = this->storage->update(PERMISSION_UUID, request.header().db_uuid(), perms.toStyledString()); result != bzn::storage_result::ok)
            {
                throw std::runtime_error("Failed to update database permissions: " + bzn::storage_result_msg.at(result));
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


void
crud::handle_remove_writers(const bzn::caller_id_t& caller_id, const database_msg& request, std::shared_ptr<bzn::session_base> session)
{
    bzn::storage_result result{bzn::storage_result::db_not_found};

    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    auto [db_exists, perms] = this->get_database_permissions(request.header().db_uuid());

    if (db_exists)
    {
        if (!this->is_caller_owner(caller_id, perms))
        {
            result = bzn::storage_result::access_denied;
        }
        else
        {
            this->remove_writers(request, perms);

            LOG(debug) << "updating db perms: " << perms.toStyledString().substr(0, MAX_MESSAGE_SIZE) << "...";

            if (result = this->storage->update(PERMISSION_UUID, request.header().db_uuid(), perms.toStyledString()); result != bzn::storage_result::ok)
            {
                throw std::runtime_error("Failed to update database permissions: " + bzn::storage_result_msg.at(result));
            }
        }
    }

    this->send_response(request, result, database_response(), session);
}


std::pair<bool, Json::Value>
crud::get_database_permissions(const bzn::uuid_t& uuid) const
{
    // does the db exist?
    if (this->storage->has(PERMISSION_UUID, uuid))
    {
        auto perms_data = this->storage->read(PERMISSION_UUID, uuid);

        if (!perms_data)
        {
            throw std::runtime_error("Failed to read database permission data!");
        }

        Json::CharReaderBuilder rbuilder;
        std::unique_ptr<Json::CharReader> const reader(rbuilder.newCharReader());
        std::string parse_errors;

        Json::Value json;

        if (!reader->parse((*perms_data).c_str(), (*perms_data).c_str() + (*perms_data).size(), &json, &parse_errors))
        {
            throw std::runtime_error("Failed to parse database json permission data: " + parse_errors);
        }

        return {true, json};
    }

    return {false, Json::Value()};
}


Json::Value
crud::create_permission_data(const bzn::caller_id_t& caller_id, const database_create_db& request) const
{
    Json::Value json;

    json[OWNER_KEY] = boost::trim_copy(caller_id);
    json[WRITERS_KEY] = Json::Value(Json::arrayValue);
    json[MAX_SIZE_KEY] = request.max_size();
    json[EVICTION_POLICY_KEY] = uint16_t(request.eviction_policy());

    LOG(debug) << "created db perms: " << json.toStyledString();

    return json;
}


bzn::value_t
crud::update_permission_data(Json::Value& perms, const database_create_db& request) const
{
    perms[MAX_SIZE_KEY] = request.max_size();
    perms[EVICTION_POLICY_KEY] = uint16_t(request.eviction_policy());

    LOG(debug) << "update db perms: " << perms.toStyledString();

    return perms.toStyledString();
}


bool
crud::is_caller_owner(const bzn::caller_id_t& caller_id, const Json::Value& perms) const
{
    return perms[OWNER_KEY] == boost::trim_copy(caller_id);
}


bool
crud::is_caller_a_writer(const bzn::caller_id_t& caller_id, const Json::Value& perms) const
{
    for (const auto& writer_id : perms[WRITERS_KEY])
    {
        if (writer_id == boost::trim_copy(caller_id))
        {
            return true;
        }
    }

    // A node may be issuing an operation such as delete for key expiration...
    // TODO: this may need to compare against all recent peers, not just current ones
    for (const auto& peer_uuid : *this->pbft->peers()->current())
    {
        if (peer_uuid.uuid == boost::trim_copy(caller_id))
        {
            return true;
        }
    }

    return this->is_caller_owner(caller_id, perms);
}


std::shared_ptr<policy::eviction_base>
crud::get_eviction_policy(const Json::Value& perms)
{
    // TODO: As we add more policies we may want to turn this into the strategy pattern and use
    // a registry based approach here
    if (perms[EVICTION_POLICY_KEY] == database_create_db::RANDOM)
    {
        return std::make_shared<policy::random>(this->storage);
    }
    else if (perms[EVICTION_POLICY_KEY] == database_create_db::VOLATILE_TTL)
    {
        return std::make_shared<policy::volatile_ttl>(this->storage);
    }

    return nullptr;
}


uint64_t
crud::max_database_size(const Json::Value& perms) const
{
    return boost::lexical_cast<uint64_t>(perms[MAX_SIZE_KEY]);
}


void
crud::add_writers(const database_msg& request, Json::Value& perms)
{
    const std::string owner = perms[OWNER_KEY].asString();

    std::set<std::string> current_writers;

    for (const auto& writer : perms[WRITERS_KEY])
    {
        current_writers.emplace(writer.asString());
    }

    for (const auto& writer : request.add_writers().writers())
    {
        // owner never should be in the writers list...
        if (writer != owner)
        {
            current_writers.insert(writer);
        }
    }

    perms[WRITERS_KEY].clear();

    for (auto&& writer : current_writers)
    {
        perms[WRITERS_KEY].append(std::move(writer));
    }
}


void
crud::remove_writers(const database_msg& request, Json::Value& perms)
{
    std::set<std::string> current_writers;

    for (auto& writer : perms[WRITERS_KEY])
    {
        current_writers.emplace(writer.asString());
    }

    for (const auto& writer : request.remove_writers().writers())
    {
        current_writers.erase(writer);
    }

    perms[WRITERS_KEY].clear();

    for (auto&& writer : current_writers)
    {
        perms[WRITERS_KEY].append(std::move(writer));
    }
}


bool
crud::save_state()
{
    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    return this->storage->create_snapshot();
}


std::shared_ptr<std::string>
crud::get_saved_state()
{
    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    return this->storage->get_snapshot();
}


bool
crud::load_state(const std::string& state)
{
    std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

    return this->storage->load_snapshot(state);
}


void
crud::update_expiration_entry(const bzn::key_t& generated_key, uint64_t expire)
{
    if (expire)
    {
        // now + expire seconds...
        const auto expires = boost::lexical_cast<std::string>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + expire);

        auto result = this->storage->create(TTL_UUID, generated_key, expires);

        if (result == bzn::storage_result::ok)
        {
            LOG(debug) << "created ttl entry [" << expires << "] for: " << generated_key;

            return;
        }

        result = this->storage->update(TTL_UUID, generated_key, expires);

        if (result != bzn::storage_result::ok)
        {
            throw std::runtime_error("Failed to update ttl entry for: " + generated_key);
        }

        return;
    }

    LOG(debug) << "removing old entry for: " << generated_key;

    this->remove_expiration_entry(generated_key);
}


void
crud::remove_expiration_entry(const bzn::key_t& generated_key)
{
    this->storage->remove(TTL_UUID, generated_key);
}


bool
crud::expired(const bzn::uuid_t& uuid, const bzn::key_t& key)
{
    auto result = this->storage->read(TTL_UUID, generate_expire_key(uuid, key));

    if (result)
    {
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const uint64_t expire = boost::lexical_cast<uint64_t>(*result);

        return (expire <= now);
    }

    return false;
}


std::optional<uint64_t>
crud::get_ttl(const bzn::uuid_t& uuid, const bzn::key_t& key) const
{
    const auto result = this->storage->read(TTL_UUID, generate_expire_key(uuid, key));

    if (result)
    {
        const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const uint64_t expire = boost::lexical_cast<uint64_t>(*result);

        if (expire > now)
        {
            return {boost::lexical_cast<uint64_t>(*result) -
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
        }

        return {0};
    }

    return {};
}


void
crud::check_key_expiration(const boost::system::error_code& ec)
{
    if (!ec)
    {
        std::lock_guard<std::shared_mutex> lock(this->crud_lock); // lock for write access

        const uint64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        for (const auto& generated_key : this->storage->get_keys(TTL_UUID))
        {
            auto result = this->storage->read(TTL_UUID, generated_key);

            if (result)
            {
                const auto [uuid, key] = extract_uuid_key(generated_key);

                // has entry expired?
                if (now >= boost::lexical_cast<uint64_t>(*result))
                {
                    LOG(debug) << "removing expired ttl entry and key for: " << uuid << ":" << key;

                    // Issue delete using pbft...
                    database_msg request;
                    request.mutable_header()->set_db_uuid(uuid);
                    request.mutable_delete_()->set_key(key);

                    bzn_envelope msg;
                    msg.set_sender(this->pbft->get_uuid());
                    msg.set_database_msg(request.SerializeAsString());

                    this->pbft->handle_database_message(msg, nullptr);
                }
                else
                {
                    // if key no longer exists, then remove the entry...
                    if (!this->storage->has(uuid, key))
                    {
                        LOG(debug) << "removing stale ttl entry for: " << uuid << ":" << key;

                        this->storage->remove(TTL_UUID, generated_key);
                    }
                }
            }
            else
            {
                std::runtime_error("Failed to read TTL value for: " + generated_key);
            }
        }

        this->expire_timer->expires_from_now(TTL_TICK);
        this->expire_timer->async_wait(std::bind(&crud::check_key_expiration, shared_from_this(), std::placeholders::_1));
    }
}


void
crud::flush_expiration_entries(const bzn::uuid_t& uuid)
{
    for (const auto& generated_key : this->storage->get_keys(TTL_UUID))
    {
        const auto [db_uuid, key] = extract_uuid_key(generated_key);

        if (db_uuid == uuid)
        {
            this->storage->remove(TTL_UUID, generated_key);

            LOG(debug) << "removing ttl entry for: " << db_uuid << ":" << key;
        }
    }
}


bool
crud::operation_exceeds_available_space(const database_msg& request, const Json::Value& perms)
{
    const auto request_type = request.msg_case();
    const auto max_size = this->max_database_size(perms);

    if (request_type == database_msg::kCreateDb || request_type == database_msg::kUpdateDb)
    {
        if (!this->max_swarm_storage)
        {
            LOG(debug) << "max storage zero, ignoring: " << request.msg_case();

            return false;
        }

        if (request_type == database_msg::kCreateDb)
        {
            return (this->get_swarm_storage_usage() + max_size > this->max_swarm_storage);
        }

        if (request_type == database_msg::kUpdateDb)
        {
            Json::Value prev_perms;
            std::tie(std::ignore, prev_perms) = this->get_database_permissions(request.header().db_uuid());

            return (this->get_swarm_storage_usage() - this->max_database_size(prev_perms) + max_size > this->max_swarm_storage);
        }
    }

    // any max size set?
    if (max_size)
    {
        uint64_t size;
        std::tie(std::ignore, size) = this->storage->get_size(request.header().db_uuid());

        // test create
        if (request_type == database_msg::kCreate)
        {
            return (size + request.create().key().size() + request.create().value().size() > max_size);
        }

        // test update
        if (request_type == database_msg::kUpdate)
        {
            const auto prev_kv_size = this->storage->get_key_size(request.header().db_uuid(),
                request.update().key());

            if (prev_kv_size)
            {
                return (size - *prev_kv_size + request.update().key().size() + request.update().value().size() > max_size);
            }
        }
    }

    return false;
}


size_t
crud::get_swarm_storage_usage()
{
    const auto databases = this->storage->get_keys(PERMISSION_UUID);

    size_t current_database_max_sizes{};

    for(const auto& database : databases)
    {
        auto const result = this->storage->read(PERMISSION_UUID, database);

        if (result)
        {
            Json::CharReaderBuilder rbuilder;
            std::unique_ptr<Json::CharReader> const reader(rbuilder.newCharReader());
            std::string parse_errors;

            Json::Value json;

            if (!reader->parse(result.value().c_str(), result.value().c_str() + result.value().size(), &json, &parse_errors))
            {
                throw std::runtime_error("Failed to parse database json uuid data: " + parse_errors);
            }

            const auto max_size = this->max_database_size(json);

            LOG(debug) << "database: " << database << " " << result.value();

            current_database_max_sizes += max_size;
        }
    }

    return current_database_max_sizes;
}


bool
crud::do_eviction(const database_msg& request, size_t max_size)
{
    const auto PERMS{this->get_database_permissions(request.header().db_uuid()).second};
    if (auto eviction_policy = this->get_eviction_policy(PERMS))
    {
        auto keys_to_evict {eviction_policy->keys_to_evict(request, max_size)};
        if (keys_to_evict.empty())
        {
            return false;
        }

        std::for_each( keys_to_evict.begin(), keys_to_evict.end(),
            [&](const auto& key)
            {
                this->storage->remove(request.header().db_uuid(), key);
            });

        return true;
    }

    return false;
}


std::string
crud::get_name()
{
    return "crud";
}


bzn::json_message
crud::get_status()
{
    bzn::json_message status;

    std::shared_lock<std::shared_mutex> lock(this->crud_lock); // lock for read access

    status["max_swarm_storage"] = uint64_t(this->max_swarm_storage);
    status["swarm_storage_usage"] = uint64_t(this->get_swarm_storage_usage());

    return status;
}
