
#include <new> // for in-place constructor
#include <string>
#include <list>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <set>
#include <map>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

typedef websocketpp::server<websocketpp::config::asio> server;
typedef std::set<websocketpp::connection_hdl, std::owner_less<websocketpp::connection_hdl> > con_list;


// a bunch of likely Max includes:
extern "C" {
	#include "ext.h"
	#include "ext_obex.h"
	#include "ext_dictionary.h"
	#include "ext_dictobj.h"
	#include "ext_strings.h"
	#include "ext_systhread.h"
}

// TODO: check any multi-threading issues?

class ws;
class Server;
static std::map <t_atom_long, Server *> server_map;

static t_class * max_class = 0;

class Server {
	
	// this can throw an exception.
	Server(t_atom_long port) : port(port) {
		server.set_open_handler(bind(&Server::on_open,this,websocketpp::lib::placeholders::_1));
		server.set_close_handler(bind(&Server::on_close,this,websocketpp::lib::placeholders::_1));
		server.set_message_handler(bind(&Server::on_message,this,websocketpp::lib::placeholders::_1,websocketpp::lib::placeholders::_2));
		server.set_reuse_addr(true);
		server.init_asio();
		server.listen(port);
		post("created server listening on port %i", port);
		server.start_accept();
		server.clear_access_channels(websocketpp::log::alevel::all); // this will turn off everything in console output
		
		received_dict_name = symbol_unique();
		received_dict = dictionary_new();
		atom_setsym(&received_dict_name_atom, received_dict_name);
		
		// make sure this doesn't get duplicated:
		server_map[port] = this;
	}
	
public:
	server server;
	t_atom_long port;
	con_list clients;				// a set of client client_connections as websocketpp::connection_hdl
	
	std::list<ws *> maxobjects;		// the set of max objects using this server
	
	t_dictionary * received_dict;
	t_symbol * received_dict_name;
	t_atom received_dict_name_atom;

	
	~Server() {
		post("closing server on port %d", port);
		server.stop_listening();
		
		// remove from map:
		const auto& it= server_map.find(port);
		server_map.erase(it);
		
		for (auto& client : clients) {
			try{
				server.close(client, websocketpp::close::status::normal, "");
			} catch (std::exception& ec) {
				error("close error %s", ec.what());
			}
		}
		
		// now run once to clear them:
		server.run();
		
		object_release((t_object *)received_dict);
	}
	
	// this can throw an exception:
	static Server * get(t_atom_long port) {
		// does a server already exist for this port?
		auto existing_server = server_map.find(port);
		if (existing_server != server_map.end()) {
			// just use that one:
			return existing_server->second;
		} else {
			// start a new one:
			return new Server(port);
		}
	}
	
	void add(ws * listener) {
		maxobjects.push_back(listener);
	}
	
	void remove(ws * listener) {
		maxobjects.remove(listener);
		
		// check whether to destroy the server at this point if there are no more listeners
		if (maxobjects.empty()) {
			delete this;
		}
	}
	
	void bang() {
		int limit = 100; // just a safety net to make sure we don't block Max due to too much IO
		while (limit-- && server.poll_one()) {};
	}
	
	void send(const std::string& msg) {
		for (auto client : clients) {
			server.send(client, msg, websocketpp::frame::opcode::text);
		}
	}
	
	void on_open(websocketpp::connection_hdl hdl) {
		clients.insert(hdl);
	}
	
	void on_close(websocketpp::connection_hdl hdl) {
		clients.erase(hdl);
	}
	
	void on_message(websocketpp::connection_hdl hdl, server::message_ptr msg);
};


class ws {
public:
	t_object ob; // max objwst, must be first!
	
	void * outlet_frame;
	void * outlet_msg;
	
	t_atom_long port;
	
	Server * server;
	
	ws() {
		outlet_msg = outlet_new(&ob, 0);
		outlet_frame = outlet_new(&ob, 0);
		port = 8080;
		server = 0;
	}
	
	void post_attr_init() {
		// will either get an existing or create a new server:
		try {
			server = Server::get(port); // can throw
			server->add(this);
		} catch (const std::exception& ex) {
			object_error(&ob, "failed to create server: %s", ex.what());
		}
	}
	
	~ws() {
		if (server) server->remove(this);
	}
	
	void bang() {
		if (server) server->bang();
	}
	
	void send(const std::string& s) {
		if (server) server->send(s);
	}
};

void Server::on_message(websocketpp::connection_hdl hdl, server::message_ptr msg) {
	
	const char * buf = msg->get_payload().c_str();
	
//	// attempt to parse as dict?
//	//dictobj_unregister(received_dict);
//	char errstring[256];
//	t_dictionary * result = NULL;
//	if (0 == dictobj_dictionaryfromstring(&result, buf, 1, errstring)) {
//		object_release((t_object *)received_dict);
//		received_dict = result;
//		dictobj_register(received_dict, &received_dict_name);
//		
//		for (auto x : maxobjects) {
//			outlet_anything(x->outlet_frame, _sym_dictionary, 1, &received_dict_name_atom);
//		}
//	} else {
//		error("error parsing received message as JSON: %s", errstring);
	
		// just output as string:
		t_symbol * sym = gensym(buf);
		for (auto x : maxobjects) {
			outlet_anything(x->outlet_frame, sym, 0, 0);
		}
	
//	}
}


void * ws_new(t_symbol *s, long argc, t_atom *argv) {
	ws *x = NULL;
	if ((x = (ws *)object_alloc(max_class))) {
		x = new (x) ws();
		// apply attrs:
		attr_args_process(x, (short)argc, argv);
		// invoke any initialization after the attrs are set from here:
		x->post_attr_init();
	}
	return (x);
}

void ws_free(ws *x) {
	x->~ws();
}

void ws_assist(ws *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { // inlet
		sprintf(s, "bang (to poll), send");
	}
	else {	// outlet
		switch (a) {
			case 0: sprintf(s, "received messages"); break;
			case 1: sprintf(s, "status messages"); break;
		}
	}
}

void ws_bang(ws * x) {
	x->bang();
}

void ws_send(ws * x, t_symbol * s) {
	x->send(std::string(s->s_name));
}

void ws_dictionary(ws * x, t_symbol * s) {
	t_dictionary *d = dictobj_findregistered_retain(s);
	if (d) {
		t_object *jsonwriter = (t_object *)object_new(_sym_nobox, _sym_jsonwriter);
		t_handle json;
		object_method(jsonwriter, _sym_writedictionary, d);
		object_method(jsonwriter, _sym_getoutput, &json);
		
		x->send(std::string(*json));
		
		object_free(jsonwriter);
		sysmem_freehandle(json);
	} else {
		object_error(&x->ob, "unable to reference dictionary named %s", s->s_name);
		return;
	}
	dictobj_release(d);
}

void ws_anything(ws * x, t_symbol * s, int argc, t_atom * argv) {
	static const char * separator = " "; // TODO we could have an attribute to set this separator character.
	
	std::stringstream ss;
	
	if (s && s->s_name) {
		ss << s->s_name;
		if (argc) ss << separator;
	}
	
	for (int i = 0; i < argc; i++,argv++) {
		switch (argv->a_type) {
			case A_LONG:
				ss << atom_getlong(argv);
				break;
			case A_FLOAT:
				ss << atom_getfloat(argv);
				break;
			case A_SYM:
				ss << atom_getsym(argv)->s_name;
				break;
			default:
				continue;
		}
		if (i < argc-1) ss << separator;
	}
	
	x->send(ss.str());
}


void ws_list(ws * x, t_symbol * s, int argc, t_atom * argv) {
	ws_anything(x, NULL, argc, argv);
}

void ext_main(void *r)
{
	t_class *c;
	
	common_symbols_init();
	
	c = class_new("ws", (method)ws_new, (method)ws_free, (long)sizeof(ws), 0L, A_GIMME, 0);
	class_addmethod(c, (method)ws_assist, "assist", A_CANT, 0);
	class_addmethod(c, (method)ws_bang,	"bang",	0);
	class_addmethod(c, (method)ws_send,	"send",	A_SYM, 0);
	class_addmethod(c, (method)ws_dictionary, "dictionary", A_SYM, 0);
	class_addmethod(c, (method)ws_anything, "anything", A_GIMME, 0);
	class_addmethod(c, (method)ws_list, "list", A_GIMME, 0);
	
	CLASS_ATTR_LONG(c, "port", 0, ws, port);
	
	class_register(CLASS_BOX, c);
	max_class = c;
}

