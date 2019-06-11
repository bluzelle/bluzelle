// Copyright (c) 2017-2018 Bluzelle Networks
//
// This file is part of Bluzelle.
//
// Bluzelle is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include <include/bluzelle.hpp>
#include <options/options.hpp>
#include <boost/lexical_cast.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <unordered_set>


namespace
{
    const char* NO_ARGS[] = {"config_tests"};

    const std::string TEST_CONFIG_FILE = "bluzelle.json";

    const std::string DEFAULT_CONFIG_CONTENT =
        "  \"listener_address\" : \"0.0.0.0\",\n"
        "  \"listener_port\" : 49152,\n"
        "  \"bootstrap_file\" : \"peers.json\",\n"
        "  \"bootstrap_url\"  : \"example.org/peers.json\",\n"
        "  \"uuid\" : \"c05c0dff-3c27-4532-96de-36f53d8a278e\",\n"
        "  \"swarm_id\" : \"utest\",\n"
        "  \"debug_logging\" : true,"
        "  \"log_to_stdout\" : true,"
        "  \"state_dir\" : \"./daemon_state/\","
        "  \"logfile_max_size\" : \"1M\","
        "  \"logfile_rotation_size\" : \"2M\","
        "  \"logfile_dir\" : \".\","
        "  \"signed_key\" : \"Oo8ZlDQcMlZF4hqnhN/2D...hoEgc0jRUl1b9mHSY7E4puk=\","
        "  \"owner_public_key\" : \"MCwwDQYJKoZIhvcNAQEBBQADGwAwGAIRAKb7PX3Pr+LgaqIAyhcXgTMCAwEAAQ==\","
        "  \"mem_storage\" : false,"
        "  \"swarm_info_esr_address\" : \"this_would_be_a_good_ESR_address\","
        "  \"swarm_info_esr_url\" : \"192.0.0.1:41000\"";

    const std::string DEFAULT_CONFIG_DATA = "{" + DEFAULT_CONFIG_CONTENT + "}";

    std::string compose_config_data(const std::string& a, const std::string& b)
    {
        std::string result = "{" + a + ",\n" + b + "}";
        return result;
    }

    const auto DEFAULT_LISTENER = boost::asio::ip::tcp::endpoint{boost::asio::ip::address::from_string("0.0.0.0"), 49152};

    void config_text_to_json(bzn::json_message& json)
    {
        std::string config_data{DEFAULT_CONFIG_DATA};
        std::string errors;

        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        reader->parse(
                DEFAULT_CONFIG_DATA.c_str()
                , DEFAULT_CONFIG_DATA.c_str() + DEFAULT_CONFIG_DATA.size()
                , &json
                , &errors);
        delete reader;
    }
}

using namespace ::testing;


// create default options and remove when done...
class options_file_test : public Test
{
public:

    std::unordered_set<std::string> open_files;

    options_file_test()
    {
        this->save_options_file(DEFAULT_CONFIG_DATA);
    }

    ~options_file_test()
    {
        for(const auto& file : this->open_files)
        {
            unlink(file.c_str());
        }
    }

    void save_file(const std::string& filename, const std::string& content)
    {
        if (this->open_files.count(filename) > 0)
        {
            unlink(TEST_CONFIG_FILE.c_str());
        }
        else
        {
            this->open_files.insert(filename);
        }

        //std::cout << filename;

        std::ofstream ofile(filename.c_str());
        ofile.exceptions(std::ios::failbit);
        ofile << content;
    }

    void
    save_options_file(const std::string& content)
    {
        save_file(TEST_CONFIG_FILE, content);
    }
};

TEST_F(options_file_test, test_that_missing_arguments_fail)
{
    this->save_options_file("{}");

    bzn::options options;

    EXPECT_THROW(options.parse_command_line(1, NO_ARGS), std::exception);
}


TEST_F(options_file_test, test_that_loading_of_default_config_file)
{
    bzn::options options;

    options.parse_command_line(1, NO_ARGS);

    EXPECT_EQ(DEFAULT_LISTENER, options.get_listener());
    ASSERT_EQ(true, options.get_debug_logging());
    ASSERT_EQ(true, options.get_log_to_stdout());
    EXPECT_EQ("utest", options.get_swarm_id());
    EXPECT_EQ("./daemon_state/", options.get_state_dir());
    EXPECT_EQ("peers.json", options.get_bootstrap_peers_file());
    EXPECT_EQ("example.org/peers.json", options.get_bootstrap_peers_url());
    EXPECT_EQ(size_t(0), options.get_max_swarm_storage());
    EXPECT_EQ(size_t(1048576), options.get_logfile_max_size());
    EXPECT_EQ(size_t(2097152), options.get_logfile_rotation_size());
    EXPECT_EQ(".", options.get_logfile_dir());
    EXPECT_FALSE(options.peer_validation_enabled());
    EXPECT_FALSE(options.get_mem_storage());
    EXPECT_EQ("Oo8ZlDQcMlZF4hqnhN/2D...hoEgc0jRUl1b9mHSY7E4puk=",options.get_signed_key());
    EXPECT_EQ("MCwwDQYJKoZIhvcNAQEBBQADGwAwGAIRAKb7PX3Pr+LgaqIAyhcXgTMCAwEAAQ==", options.get_owner_public_key());
    EXPECT_EQ("this_would_be_a_good_ESR_address", options.get_swarm_info_esr_address());
    EXPECT_EQ("192.0.0.1:41000", options.get_swarm_info_esr_url());

    // defaults..
    {
        bzn::options options0;
        this->save_options_file("{}");

        // Will fail without many required arguments, but we can still get default values where they exist
        EXPECT_THROW(options0.parse_command_line(1, NO_ARGS), std::exception);

        EXPECT_EQ("./.state/", options0.get_state_dir());
        EXPECT_EQ(size_t(524288), options0.get_logfile_max_size());
        EXPECT_EQ(size_t(65536), options0.get_logfile_rotation_size());
        EXPECT_EQ("logs/", options0.get_logfile_dir());
        EXPECT_TRUE(options0.get_mem_storage());
        EXPECT_EQ("", options0.get_swarm_id());
        EXPECT_EQ(bzn::utils::DEFAULT_SWARM_INFO_ESR_ADDRESS, options0.get_swarm_info_esr_address());
        EXPECT_EQ(bzn::utils::ROPSTEN_URL, options0.get_swarm_info_esr_url());
    }
}


TEST(options, test_that_missing_default_config_throws_exception)
{
    bzn::options options;

    EXPECT_THROW(options.parse_command_line(1, NO_ARGS), std::runtime_error);
}


TEST_F(options_file_test, test_max_storage_parsing)
{
    bzn::json_message json;
    config_text_to_json(json);

    std::for_each(bzn::utils::BYTE_SUFFIXES.cbegin()
            , bzn::utils::BYTE_SUFFIXES.cend()
            , [&](const auto& p)
                  {
                      const size_t expected = 3 * 1099511627776; // 3TB in B
                      {
                          json["max_swarm_storage"] = boost::lexical_cast<std::string>(expected / p.second) + p.first;

                          this->save_options_file(json.toStyledString());

                          bzn::options options;
                          options.parse_command_line(1, NO_ARGS);

                          EXPECT_EQ(expected, options.get_max_swarm_storage());
                      }
                      {
                          std::string max_storage{boost::lexical_cast<std::string>(expected / p.second)};
                          max_storage = max_storage + p.first;
                          if (p.first!='B')
                          {
                              max_storage = max_storage.append("B");
                          }
                          json["max_swarm_storage"] = max_storage;

                          this->save_options_file(json.toStyledString());

                          bzn::options options;
                          options.parse_command_line(1, NO_ARGS);

                          EXPECT_EQ(expected, options.get_max_swarm_storage());
                      }
                  });

}


TEST_F(options_file_test, test_enable_whitlelist_temporary)
{
    bzn::json_message json;
    config_text_to_json(json);
    {
        json[bzn::option_names::PEER_VALIDATION_ENABLED] = false;
        this->save_options_file(json.toStyledString());
        bzn::options options;
        options.parse_command_line(1, NO_ARGS);
        EXPECT_FALSE(options.peer_validation_enabled());
    }
    {
        json[bzn::option_names::PEER_VALIDATION_ENABLED] = true;
        this->save_options_file(json.toStyledString());
        bzn::options options;
        options.parse_command_line(1, NO_ARGS);
        EXPECT_TRUE(options.peer_validation_enabled());
    }
}


TEST_F(options_file_test, test_that_command_line_options_work)
{
    bzn::options options;
    const char* ARGS[] = {"swarm", "-c", TEST_CONFIG_FILE.c_str()};
    options.parse_command_line(3, ARGS);
    std::cout << options.get_bootstrap_peers_file() << std::endl;
    EXPECT_EQ(DEFAULT_LISTENER, options.get_listener());
    ASSERT_EQ(true, options.get_debug_logging());
    ASSERT_EQ(true, options.get_log_to_stdout());
    EXPECT_EQ("./daemon_state/", options.get_state_dir());
    EXPECT_EQ("peers.json", options.get_bootstrap_peers_file());
    EXPECT_EQ("example.org/peers.json", options.get_bootstrap_peers_url());
    EXPECT_EQ(size_t(0), options.get_max_swarm_storage());
    EXPECT_EQ(size_t(1048576), options.get_logfile_max_size());
    EXPECT_EQ(size_t(2097152), options.get_logfile_rotation_size());
    EXPECT_EQ(".", options.get_logfile_dir());
    EXPECT_FALSE(options.peer_validation_enabled());
    EXPECT_EQ("MCwwDQYJKoZIhvcNAQEBBQADGwAwGAIRAKb7PX3Pr+LgaqIAyhcXgTMCAwEAAQ==", options.get_owner_public_key());
    EXPECT_EQ("this_would_be_a_good_ESR_address", options.get_swarm_info_esr_address());
}

TEST_F(options_file_test, test_that_no_monitor_endpoint_when_not_specified)
{
    bzn::options options;

    try
    {
        options.parse_command_line(1, NO_ARGS);
    }
    catch (const std::exception& e)
    {
        // We are missing some required arguments, that's fine, we want to test against defaults
    }

    auto io_context = std::make_shared<bzn::asio::io_context>();

    EXPECT_EQ(options.get_monitor_endpoint(io_context), std::optional<boost::asio::ip::udp::endpoint>{});
}

TEST_F(options_file_test, test_that_endpoint_built)
{
    bzn::options options;
    this->save_options_file("{\""
                            + bzn::option_names::MONITOR_ADDRESS + "\": \"localhost\", \""
                            + bzn::option_names::MONITOR_PORT + "\": 12345}");

    try
    {
        options.parse_command_line(1, NO_ARGS);
    }
    catch (const std::exception& e)
    {
        // We are missing some required arguments, that's fine, we want to test against what we've specified
    }

    auto io_context = std::make_shared<bzn::asio::io_context>();

    boost::asio::ip::udp::resolver resolver(io_context->get_io_context());
    auto eps = resolver.resolve(boost::asio::ip::udp::v4(), "localhost", "12345");
    auto expect = std::optional<boost::asio::ip::udp::endpoint>{*eps.begin()};

    EXPECT_EQ(options.get_monitor_endpoint(io_context), expect);
}

TEST_F(options_file_test, test_that_pubkey_used_for_uuid)
{
    bzn::options options;
    this->save_options_file("{\"public_key_file\": \"pkey.pem\"}");

    this->save_file("pkey.pem",
            "-----BEGIN PUBLIC KEY-----\n"
            "hFWG\n"
            "-----END PUBLIC KEY-----\n"
    );

    try
    {
        options.parse_command_line(1, NO_ARGS);
    }
    catch (const std::exception& e)
    {
    }

    EXPECT_EQ(options.get_uuid(), "hFWG");
}

TEST_F(options_file_test, test_that_uuid_and_pubkey_conflict)
{
    bzn::options options;
    this->save_options_file(compose_config_data(DEFAULT_CONFIG_CONTENT, "\"public_key_file\": \"somefile\""));
    EXPECT_FALSE(options.parse_command_line(1, NO_ARGS));
}

TEST_F(options_file_test, test_set_option_at_runtime)
{
    bzn::options options;
    this->save_options_file(DEFAULT_CONFIG_DATA);
    EXPECT_TRUE(options.parse_command_line(1, NO_ARGS));

    options.get_mutable_simple_options().set(bzn::option_names::DEBUG_LOGGING, "false");
    EXPECT_FALSE(options.get_simple_options().get<bool>(bzn::option_names::DEBUG_LOGGING));

    options.get_mutable_simple_options().set(bzn::option_names::DEBUG_LOGGING, "true");
    EXPECT_TRUE(options.get_simple_options().get<bool>(bzn::option_names::DEBUG_LOGGING));

    options.get_mutable_simple_options().set(bzn::option_names::DEBUG_LOGGING, "false");
    EXPECT_FALSE(options.get_simple_options().get<bool>(bzn::option_names::DEBUG_LOGGING));
}
