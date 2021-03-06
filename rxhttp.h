#pragma once


struct progress_t
{
    int bytesLoaded;
    int totalSize;
};
struct response_t 
{
    struct state_t : public enable_shared_from_this<state_t>
    {
        state_t(std::string url) 
            : url(url)
            , loadhub({})
            , progresshub(progress_t{0, 0}) 
            {}
        std::string url;
        behavior<vector<uint8_t>> loadhub;
        behavior<progress_t> progresshub;
    };
    shared_ptr<state_t> state;
    response_t(std::string url) 
        : state(make_shared<state_t>(url))
        {}
    string url() const {return state->url;}
    observable<progress_t> progress() const {
        return state->progresshub.get_observable();
    }
    observable<vector<uint8_t>> load() const {
        return state->loadhub.get_observable();
    }
    void abort() const {
        state->loadhub.get_subscriber().unsubscribe();
    }
};
struct http_status_exception : public exception
{
    http_status_exception(int code, const char* m) : code(code), message(m) {}
    int code;
    string message;
    const char* what() const noexcept {return message.c_str();}
    static http_status_exception from(exception_ptr ep) {
        try { rethrow_exception(ep); }
        catch(http_status_exception he) {
            return he;
        }
    }
};
observable<response_t> httpGet(const char* urlArg)
{
    std::string url = urlArg;
    return create<response_t>([=](subscriber<response_t> dest){
        auto response = response_t(url);
        
        int token = emscripten_async_wget2_data(
            response.url().c_str(),
            "GET",
            "",
            response.state.get(),
            true, // the buffer is freed when unload returns
            [](unsigned, void* vp, void* d, unsigned s){
                auto state = reinterpret_cast<response_t::state_t*>(vp);
                
                state->progresshub.get_subscriber().on_completed();

                auto begin = reinterpret_cast<uint8_t*>(d);
                vector<uint8_t> data{begin, begin + s};
                state->loadhub.get_subscriber().on_next(data);
                state->loadhub.get_subscriber().on_completed();
            },
            [](unsigned, void* vp, int code, const char* m){
                auto state = reinterpret_cast<response_t::state_t*>(vp);

                state->progresshub.get_subscriber().on_completed();

                on_exception(
                    [=](){throw http_status_exception(code, m); return 0;}, 
                    state->loadhub.get_subscriber());
            },
            [](unsigned, void* vp, int p, int t){
                auto state = reinterpret_cast<response_t::state_t*>(vp);

                state->progresshub.get_subscriber().on_next(progress_t{p, t});
            });

        response.state->loadhub.get_subscriber().add([token, response](){
            emscripten_async_wget2_abort(token);
        });
        
        dest.on_next(response);
        dest.on_completed();
        
    });
}

struct model {
    struct data {
        int size;
        string line;
    };
    std::map<string, data> store;
};
std::ostream& operator<< (std::ostream& out, const model& m) {
    for (auto i : m.store) {
        auto url = i.first;
        auto d = i.second;
        size_t sep = url.find_last_of("\\/");
        if (sep != std::string::npos)
            url = url.substr(sep + 1, url.size() - sep - 1);
        out << url << ", " << d.size;
        if (!d.line.empty()) {
            out << endl << d.line;
        }
    }
    return out;
}

extern"C" {
    void rxhttp(const char*);
}

void rxhttp(const char* url){
    httpGet(url).
        map([](response_t response){
            return response.
                progress().
                start_with(progress_t{0,0}).
                combine_latest(
                    [=](progress_t p, vector<uint8_t> d){
                        return make_tuple(response.url(), p, d);
                    },
                    response.load().start_with(vector<uint8_t>{})).
                scan(
                    model{}, 
                    [](model m, tuple<string, progress_t, vector<uint8_t>> u){
                        apply(u, [&](string url, progress_t p, vector<uint8_t> d) {
                            auto& data = m.store[url];
                            data.line.assign(d.begin(), find(d.begin(), d.end(), '\n'));
                            data.size = max(p.bytesLoaded, int(d.size()));
                        });
                        return m;
                    });
        }).
        merge().
        subscribe(
            lifetime,
            println(cout), 
            [](exception_ptr ep){cout << endl << "error: " << http_status_exception::from(ep).code << endl;});
}
