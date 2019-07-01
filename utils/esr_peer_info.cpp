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

#include <options/options.hpp>
#include <utils/http_req.hpp>
#include <utils/esr_peer_info.hpp>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>
#include <json/json.h>
#include <iostream>
#include <sstream>

namespace
{
    const std::string ERR_UNABLE_TO_PARSE_JSON_RESPONSE{"Unable to parse JSON response: "};
    const std::string GET_NODE_LIST_ABI{R"(
        {
           "constant":true,
           "inputs":[
              {
                 "name":"swarmID",
                 "type":"string"
              }
           ],
           "name":"getNodeList",
           "outputs":[
              {
                 "name":"",
                 "type":"string[]"
              }
           ],
           "payable":false,
           "stateMutability":"view",
           "type":"function",
           "signature":"0x46e76d8b"
        }
    )"};

    const std::string GET_PEER_INFO_ABI{R"(
        {
           "constant":true,
           "inputs":[
              {
                 "name":"swarmID",
                 "type":"string"
              },
              {
                 "name":"nodeUUID",
                 "type":"string"
              }
           ],
           "name":"getNodeInfo",
           "outputs":[
              {
                 "name":"nodeCount",
                 "type":"uint256"
              },
              {
                 "name":"nodeHost",
                 "type":"string"
              },
              {
                 "name":"nodeHttpPort",
                 "type":"uint256"
              },
              {
                 "name":"nodeName",
                 "type":"string"
              },
              {
                 "name":"nodePort",
                 "type":"uint256"
              }
           ],
           "payable":false,
           "stateMutability":"view",
           "type":"function",
           "signature":"0xcc8575cb"
        }
    )"};

    const size_t ESR_RESPONSE_LINE_LENGTH{64};


    void
    trim_right_nulls(std::string& s)
    {
        boost::algorithm::trim_right_if(s, [](auto &c){return c=='\0';});
    }


    bzn::json_message
    str_to_json(const std::string &json_str)
    {
        bzn::json_message json_msg;
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        std::string errors;
        if(!reader->parse(
                json_str.c_str()
                , json_str.c_str() + json_str.size()
                , &json_msg
                , &errors))
        {
            throw (std::runtime_error(ERR_UNABLE_TO_PARSE_JSON_RESPONSE + errors));
        }
        return json_msg;
    }


    bzn::json_message
    make_params(const std::string& to_hex, const std::string& data_hex)
    {
        bzn::json_message params;
        bzn::json_message param;
        param["to"] = "0x" + to_hex;
        param["data"] = data_hex;
        params.append(param);
        params.append("latest");
        return params;
    }


    std::string
    make_request(const std::string& to_hex, const std::string& data_hex)
    {
        bzn::json_message request;
        request["jsonrpc"] = "2.0";
        request["method"] = "eth_call";
        request["params"] = make_params(to_hex, data_hex);
        request["id"] = 1;
        Json::StreamWriterBuilder wbuilder;
        wbuilder["indentation"] = "";
        return Json::writeString(wbuilder, request);
    }


    std::string
    hex_to_char_string(const std::string &hex)
    {
        std::stringstream strm;
        boost::algorithm::unhex(hex, std::ostream_iterator<char>{strm, ""});
        return strm.str();
    }


    // TODO: replace this with a function that uses the ABI to parse the response
    std::vector<std::string>
    parse_get_peers_result_to_vector(const std::string_view &result)
    {
        std::vector<std::string> results;
        std::vector<std::string> lines;
        size_t node_count{0};
        enum states {HEADER, HEADER_SWARM_SIZE, HEADER_INFO, PEER_ID_SIZE, PEER_ID } state{HEADER};

        // NOTE: I needed to add the extra null character to "line" so that any 64 length buffer read from the result
        // will be correctly null terminated for the boost::algorithm::unhex function
        char line[ESR_RESPONSE_LINE_LENGTH + 1]{0};
        size_t index{0};
        std::string peer_id;

        std::istringstream stm(result.begin());

        size_t peer_id_length{0};
        while (stm.read(line, ESR_RESPONSE_LINE_LENGTH))
        {
            switch (state)
            {
                case HEADER:
                {
                    state = HEADER_SWARM_SIZE;
                }
                break;
                case HEADER_SWARM_SIZE:
                {
                    state = HEADER_INFO;
                    node_count = std::strtoul(line, nullptr, 16);
                    if (node_count == 0)
                    {
                        LOG(error) << "Requested swarm may not exist or has no nodes";
                        return results;
                    }
                }
                break;
                case HEADER_INFO:
                {
                    state = (index == node_count + 1 ? PEER_ID_SIZE : state) ;
                }
                break;
                case PEER_ID_SIZE:
                {
                    // the first line of data is the size of the string
                    peer_id_length = std::strtoul(line, nullptr, 16);
                    if (peer_id_length)
                    {
                        state = PEER_ID;
                    }
                    else
                    {
                        --node_count;
                        state = PEER_ID_SIZE;
                    }
                }
                break;
                case PEER_ID:
                {
                    peer_id.append(hex_to_char_string(std::string{line}));
                    if (peer_id.size() >= peer_id_length)
                    {
                        trim_right_nulls(peer_id);
                        results.emplace_back(peer_id);
                        peer_id = "";
                        state = PEER_ID_SIZE;
                    }
                }
                break;
                default:
                {
                    LOG(warning) << "Failed to correctly parse peer peers from esr";
                    return results;
                }
                break;
            }
            ++index;
        }

        // TODO: rhn - reconsider if we need to keep node_count and this check.
        if (results.size() != node_count)
        {
            LOG(warning) << "Actual size of the peers list [" << results.size() << "] does not agree with the expected size [" << node_count << "]";
        }

        return results;
    }


    // TODO: replace this with a function that uses the ABI to parse the response
    bzn::peer_address_t
    parse_get_peer_info_result_to_peer_address(const std::string &peer_id, const std::string_view &result)
    {
        size_t      text_size{0};
        uint16_t    port{0};
        std::string host;
        std::string name;
        enum {NODE_COUNT, NA_0, NA_1, NODE_PORT, NODE_HOST_SIZE, NODE_HOST, NODE_NAME_SIZE, NODE_NAME, FINISHED} state {NODE_COUNT};

        // NOTE: I needed to add the extra null character to "line" so that any 64 length buffer read from the result
        // will be correctly null terminated for the boost::algorithm::unhex function
        char line[ESR_RESPONSE_LINE_LENGTH + 1]{0};
        std::istringstream stream{result.begin()};
        while(stream.read(line, ESR_RESPONSE_LINE_LENGTH))
        {
            // TODO: replace this switch/case with the strategy pattern - Rich
            switch (state)
            {
                case NODE_COUNT:
                {
                    state = NA_0;
                }
                break;

                case NA_0:
                {
                    state = NA_1;
                }
                break;

                case NA_1:
                {
                    state = NODE_PORT;
                }
                break;

                case NODE_PORT:
                {
                    port = std::strtoul(line, nullptr, 16);
                    if (!port)
                    {
                        LOG(warning) << "Invalid value for port:[" << port << "], node may not exist";
                    }
                    state = NODE_HOST_SIZE;
                }
                break;

                case NODE_HOST_SIZE:
                {
                    text_size = std::strtoul(line, nullptr, 16);
                    if (!text_size)
                    {
                        LOG(warning) << "Invalid value for host string length:[" << text_size << "]";
                    }
                    state = NODE_HOST;
                }
                break;

                case NODE_HOST:
                {
                    host = hex_to_char_string(line);
                    trim_right_nulls(host);
                    if (text_size != host.size())
                    {
                        LOG(warning) << "Parsed host string size does not match expected size";
                    }
                    state = NODE_NAME_SIZE;
                }
                break;

                case NODE_NAME_SIZE:
                {
                    text_size = std::strtoul(line, nullptr, 16);
                    if (!text_size)
                    {
                        LOG(warning) << "Invalid value for node name string length:[" << text_size << "]";
                    }
                    state = NODE_NAME;
                    name.clear();
                }
                break;

                case NODE_NAME:
                {
                    name.append(hex_to_char_string(line));
                    trim_right_nulls(name);
                    if (text_size == name.size())
                    {
                        state = FINISHED;
                    }
                }
                break;

                case FINISHED:
                {
                    LOG(warning) << "Peer Info result contains too many lines";
                }
                break;

                default:
                {
                    LOG(error) << "Failed to correctly parse peer info from esr";
                    return bzn::peer_address_t(host, port, name, peer_id);
                }
                break;
            }
        }
        return bzn::peer_address_t(host, port, name, peer_id);
    }


    std::string
    pad_str_to_mod_64(const std::string &parameter)
    {
        const size_t REQUIRED_MOD{64};
        const size_t PADDING_REQUIRED = (REQUIRED_MOD - parameter.size() % REQUIRED_MOD);
        std::string result{parameter};
        result.insert(result.size(), PADDING_REQUIRED, '0');
        return result;
    }


    std::string
    size_type_to_hex(size_t i, size_t width = 8)
    {
        std::stringbuf buf;
        std::ostream os(&buf);
        os << std::setfill('0') << std::setw(width) << std::hex << (int)i;
        return buf.str().c_str();
    }


    std::string
    string_to_hex(const std::string& value)
    {
        std::stringstream hexstream;
        boost::algorithm::hex(value, std::ostream_iterator<char>{hexstream, ""});
        return hexstream.str();
    }


    // TODO: replace this with a function that uses the ABI to create the request data
    const std::string
    data_string_for_get_peers(const std::string &swarm_id)
    {
        static const auto NODE_LIST_ABI = str_to_json(GET_NODE_LIST_ABI);
        static const auto GET_PEERS_ADDRESS{NODE_LIST_ABI["signature"].asCString() + 2}; // 0x46e76d8b -> 46e76d8b

        return std::string{"0x"
                + pad_str_to_mod_64(GET_PEERS_ADDRESS)
                + pad_str_to_mod_64("00000020")             // input parameter type? (no)
                + size_type_to_hex(swarm_id.size())         // size of the swarm id (pre hexification)
                + pad_str_to_mod_64(string_to_hex(swarm_id))// hexified swarm id
        };
    }


    // TODO: replace this with a function that uses the ABI to create the request data
    const std::string
    data_string_for_get_peer_info(const std::string &swarm_id, const std::string &peer_id)
    {
        static const auto PEER_INFO_ABI{str_to_json(GET_PEER_INFO_ABI)};
        static const auto GET_PEER_INFO_SIGNATURE{PEER_INFO_ABI["signature"].asCString() + 2};

        const std::string PARAMS
            {
                size_type_to_hex(swarm_id.size(), 64)           // size of swarm id string (pre hexification)
                + pad_str_to_mod_64(string_to_hex(swarm_id))    // parameter 1 - swarm id
                + size_type_to_hex(peer_id.size(), 64)          // size of peer id (pre hexification)
                + pad_str_to_mod_64(string_to_hex(peer_id))     // parameter 2 - peer id
            };

        return std::string{"0x"
            + pad_str_to_mod_64(GET_PEER_INFO_SIGNATURE)
            + pad_str_to_mod_64("00000040")                 // first param type?
            + size_type_to_hex(PARAMS.size() / 2)           // size of params blob in bytes, padded to 8 chars
            + PARAMS
        };
    }
}


namespace bzn::utils::esr
{
    std::vector<std::string>
    get_peer_ids(const bzn::uuid_t& swarm_id, const std::string& esr_address, const std::string& url)
    {
        const auto DATA{data_string_for_get_peers(swarm_id)};
        const auto REQUEST{make_request( esr_address, DATA)};
        const auto response{bzn::utils::http::sync_req(url, REQUEST)};
        const auto json_response{str_to_json(response)};
        const auto result{json_response["result"].asCString() + 2}; // + 2 skips the '0x'
        return parse_get_peers_result_to_vector(result);
    }


    bzn::peer_address_t
    get_peer_info(const bzn::uuid_t& swarm_id, const std::string& peer_id, const std::string& esr_address, const std::string& url)
    {
        const auto DATA{data_string_for_get_peer_info(swarm_id, peer_id)};
        const auto REQUEST{make_request( esr_address, DATA)};
        const auto response{bzn::utils::http::sync_req(url, REQUEST)};
        const auto json_response{str_to_json(response)};
        const auto result{json_response["result"].asCString() + 2};
        return parse_get_peer_info_result_to_peer_address(peer_id, result);
    }
}
