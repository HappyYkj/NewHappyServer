#pragma once
#include "common/string.hpp"
#include "common/time.hpp"
#include "common/rapidjson_helper.hpp"
#include "rapidjson/cursorstreamwrapper.h"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/en.h"

struct service_config
{
    bool unique = false;
    int32_t threadid = 0;
    std::string type;
    std::string name;
    std::string config;
};

struct server_config
{
    int32_t sid = 0;
    int32_t thread = 0;
    std::string loglevel;
    std::string name;
    std::string outer_host;
    std::string inner_host;
    std::string startup;
    std::string log;
    std::vector<std::string> path;
    std::vector<std::string> cpath;
    std::vector<service_config> services;
};

class server_config_manger
{
    server_config_manger() = default;

    bool prepare(const std::string& config)
    {
        config_.append("[");

        rapidjson::StringStream ss(config.data());
        rapidjson::CursorStreamWrapper<rapidjson::StringStream> csw(ss);
        rapidjson::Document doc;
        doc.ParseStream<rapidjson::kParseCommentsFlag>(csw);
        if (doc.HasParseError())
        {
            printf("Parse server config failed:%s(%d).line %d col %d", rapidjson::GetParseError_En(doc.GetParseError()), (int) doc.GetParseError(), (int) csw.GetLine(), (int)csw.GetColumn());
            return false;
        }

        if (!doc.IsArray())
        {
            printf("Server config format error : must be json array.");
            return false;
        }

        for (auto&c : doc.GetArray())
        {
            if (config_.size() > 1)
            {
                config_.append(",");
            }

            server_config scfg;
            scfg.sid = rapidjson::get_value<int32_t>(&c, "sid", -1);// server id
            if (scfg.sid == -1)
            {
                printf("Server config format error:must has sid");
                return false;
            }

            scfg.outer_host = rapidjson::get_value<std::string>(&c, "outer_host", "*");
            scfg.inner_host = rapidjson::get_value<std::string>(&c, "inner_host", "127.0.0.1");
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            c.Accept(writer);
            std::string s(buffer.GetString(), buffer.GetSize());
            replace(s, "#sid", std::to_string(scfg.sid));
            replace(s, "#outer_host", scfg.outer_host);
            replace(s, "#inner_host", scfg.inner_host);
            config_.append(s);
        }
        config_.append("]");
        return true;
    }

public:
    static server_config_manger& instance()
    {
        static server_config_manger obj;
        return obj;
    }

    const std::string config()
    {
        return config_;
    }

    bool parse(const std::string& config, int32_t sid)
    {
        prepare(config);
        rapidjson::StringStream ss(config_.data());
        rapidjson::CursorStreamWrapper<rapidjson::StringStream> csw(ss);
        rapidjson::Document doc;
        doc.ParseStream<rapidjson::kParseCommentsFlag>(csw);

        if (doc.HasParseError())
        {
            printf("Parse server config failed:%s(%d).line %d col %d.\n", rapidjson::GetParseError_En(doc.GetParseError()), (int)doc.GetParseError(), (int)csw.GetLine(), (int)csw.GetColumn());
            return false;
        }

        if (!doc.IsArray())
        {
            printf("Server config format error: must be json array.\n");
            return false;
        }

        for (auto&c : doc.GetArray())
        {
            server_config scfg;
            scfg.sid = rapidjson::get_value<int32_t>(&c, "sid", -1);// server id
            if (scfg.sid == -1)
            {
                printf("Server config format error:must has sid.\n");
                return false;
            }

            scfg.name = rapidjson::get_value<std::string>(&c, "name");// server name
            if (scfg.name.empty())
            {
                printf("Server config format error:must has name.\n");
                return false;
            }

            scfg.outer_host = rapidjson::get_value<std::string>(&c, "outer_host", "*");
            scfg.inner_host = rapidjson::get_value<std::string>(&c, "inner_host", "127.0.0.1");
            scfg.thread = rapidjson::get_value<int32_t>(&c, "thread", std::thread::hardware_concurrency());
            scfg.startup = rapidjson::get_value<std::string>(&c, "startup");
            scfg.log = rapidjson::get_value<std::string>(&c, "log");
            scfg.loglevel = rapidjson::get_value<std::string>(&c, "loglevel", "DEBUG");
            scfg.path  = rapidjson::get_value<std::vector<std::string>>(&c, "path");
            scfg.cpath = rapidjson::get_value<std::vector<std::string>>(&c, "cpath");

            if (scfg.log.find("#date") != std::string::npos)
            {
                time_t now = std::time(nullptr);
                std::tm m;
                time::localtime(&now, &m);
                char buff[50 + 1] = { 0 };
                std::strftime(buff, 50, "%Y%m%d%H%M%S", &m);
                replace(scfg.log, "#date", buff);
            }

            if (scfg.sid == sid)
            {
                auto services = rapidjson::get_value<rapidjson::Value*>(&c, "services", nullptr);
                if (!services->IsArray())
                {
                    printf("Server config format error: services must be array.\n");
                    return false;
                }

                for (auto& s : services->GetArray())
                {
                    if (!s.IsObject())
                    {
                        printf("Server config format error: service must be object.\n");
                        return false;
                    }

                    service_config sc;
                    sc.type = rapidjson::get_value<std::string>(&s, "type", "lua");
                    sc.unique = rapidjson::get_value<bool>(&s, "unique", false);
                    sc.threadid = rapidjson::get_value<int32_t>(&s, "threadid", 0);
                    sc.name = rapidjson::get_value<std::string>(&s, "name");
      
                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    s.Accept(writer);
                    sc.config = std::string(buffer.GetString(), buffer.GetSize());

                    if (sc.config.empty())
                    {
                        printf("Server config format error: service config must not be empty.\n");
                        return false;
                    }

                    scfg.services.emplace_back(sc);
                }
            }

            if (!data_.emplace(scfg.sid, scfg).second)
            {
                printf("Server config format error : sid % d already exist.\n", scfg.sid);
                return false;
            }
        }
        sid_ = sid;
        return true;
    }

    template<typename Handler>
    void for_all(Handler&& handler)
    {
        for (auto& c : data_)
        {
            handler(c.second);
        }
    }

    server_config* find(int32_t sid)
    {
        auto iter = data_.find(sid);
        if (iter != data_.end())
        {
            return &iter->second;
        }
        return nullptr;
    }

    server_config* get_server_config()
    {
        auto iter = data_.find(sid_);
        if (iter != data_.end())
        {
            return &iter->second;
        }
        return nullptr;
    }

private:
    int sid_ = 0;
    std::string config_;
    std::unordered_map<int32_t, server_config> data_;
};
