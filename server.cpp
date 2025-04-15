#include <uWebSockets/App.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <memory>
#include <random>

using namespace std;

// Forward declarations
class Room;
class User;

using UserPtr = shared_ptr<User>;
using RoomPtr = shared_ptr<Room>;

unordered_map<string, UserPtr> userMap;
unordered_map<string, RoomPtr> roomMap;

unordered_set<string> allUsernames;
unordered_set<string> allRoomNames;

struct PerSocketData {
    string username;
};

// Room class
class Room {
public:
    string name;
    string secretKey;
    unordered_set<string> users;

    Room(string n, string key) : name(move(n)), secretKey(move(key)) {}
};

// User class
class User : public enable_shared_from_this<User> {
public:
    string username;
    string password;
    unordered_set<string> joinedRooms;
    string currentRoom;
    uWS::WebSocket<false, true, PerSocketData>* ws = nullptr;

    User(string uname, string pwd) : username(move(uname)), password(move(pwd)) {}

    bool joinRoom(const string& roomName, const string& secretKey) {
        auto it = roomMap.find(roomName);
        if (it != roomMap.end() && it->second->secretKey == secretKey) {
            joinedRooms.insert(roomName);
            currentRoom = roomName;
            it->second->users.insert(username);
            return true;
        }
        return false;
    }

    void sendMessage(const string& message) {
        if (currentRoom.empty()) return;
        auto room = roomMap[currentRoom];
        for (const auto& uname : room->users) {
            if (uname != username) {
                auto u = userMap[uname];
                if (u && u->ws) {
                    u->ws->send(username + ": " + message, uWS::OpCode::TEXT);
                }
            }
        }
    }
};

// Generate UUID for room secret key
string generateUUID() {
    static random_device rd;
    static mt19937 gen(rd());
    static uniform_int_distribution<> dis(0, 15);
    static const char* hex = "0123456789abcdef";

    string uuid;
    for (int i = 0; i < 32; ++i) {
        uuid += hex[dis(gen)];
        if (i == 7 || i == 11 || i == 15 || i == 19) uuid += "-";
    }
    return uuid;
}

// Create a user profile
bool createUser(const string& uname, const string& pwd) {
    if (userMap.find(uname) != userMap.end()) return false;
    userMap[uname] = make_shared<User>(uname, pwd);
    allUsernames.insert(uname);
    return true;
}

// Authenticate a user
UserPtr loginUser(const string& uname, const string& pwd) {
    auto it = userMap.find(uname);
    if (it != userMap.end() && it->second->password == pwd) {
        return it->second;
    }
    return nullptr;
}

// Create room and auto-join creator
string createRoom(const string& roomName, const UserPtr& creator) {
    if (roomMap.find(roomName) != roomMap.end()) return "";

    string secretKey = generateUUID();
    RoomPtr newRoom = make_shared<Room>(roomName, secretKey);
    roomMap[roomName] = newRoom;
    allRoomNames.insert(roomName);

    creator->joinedRooms.insert(roomName);
    creator->currentRoom = roomName;
    newRoom->users.insert(creator->username);

    return secretKey;
}

// WebSocket Server
int main() {
    uWS::App().ws<PerSocketData>("/*", {
        .open = [](auto* ws) {
            cout << "A client connected." << endl;
        },
        .message = [](auto* ws, string_view msg, uWS::OpCode opCode) {
            string input(msg);
            auto* data = ws->getUserData();

            size_t colon = input.find(':');
            if (colon == string::npos) {
                ws->send("Invalid format", opCode);
                return;
            }

            string cmd = input.substr(0, colon);
            string args = input.substr(colon + 1);

            if (cmd == "CREATE_USER") {
                size_t comma = args.find(',');
                if (comma == string::npos) {
                    ws->send("Invalid format for CREATE_USER", opCode);
                    return;
                }
                string uname = args.substr(0, comma);
                string pwd = args.substr(comma + 1);
                if (createUser(uname, pwd)) {
                    data->username = uname;
                    userMap[uname]->ws = ws;
                    ws->send("User created and logged in", opCode);
                }
                else {
                    ws->send("Username already exists", opCode);
                }

            }
            else if (cmd == "LOGIN") {
                size_t comma = args.find(',');
                 if (comma == string::npos) {
                     ws->send("Invalid format for LOGIN", opCode);
                     return;
                 }
                 string uname = args.substr(0, comma);
                 string pwd = args.substr(comma + 1);
                 auto user = loginUser(uname, pwd);
                 if (user) {
                     data->username = uname;
                     user->ws = ws;
                     ws->send("Login successful", opCode);
                 }
                 else {
                     ws->send("Invalid credentials", opCode);
                 }

            }
            else if (cmd == "CREATE_ROOM") {
                string rname = args;
                auto user = userMap[data->username];
                string key = createRoom(rname, user);
                if (!key.empty()) {
                    ws->send("Room created and connected. Secret: " + key, opCode);
                }
                else {
                    ws->send("Room name already exists", opCode);
                }

            }
            else if (cmd == "JOIN_ROOM") {
                size_t comma = args.find(',');
                if (comma == string::npos) {
                    ws->send("Invalid format for JOIN_ROOM", opCode);
                    return;
                }
                string rname = args.substr(0, comma);
                string key = args.substr(comma + 1);
                auto user = userMap[data->username];
                if (user->joinedRooms.count(rname)) {
                    user->currentRoom = rname;
                    ws->send("Reconnected to room " + rname, opCode);
                }
                else if (user->joinRoom(rname, key)) {
                    ws->send("Joined room " + rname, opCode);
                }
                else {
                    ws->send("Failed to join room (wrong key?)", opCode);
                }

            }
            else if (cmd == "CONNECT_ROOM") {
                string rname = args;
                auto user = userMap[data->username];

                if (!user) {
                    ws->send("You are not logged in", opCode);
                    return;
                }

                if (user->joinedRooms.count(rname)) {
                    user->currentRoom = rname;
                    ws->send("Connected to room " + rname, opCode);
                }
                else {
                    ws->send("You have not joined this room before. Use JOIN_ROOM with secret key.", opCode);
                }
            }
            else if (cmd == "SEND") {
                auto user = userMap[data->username];
                user->sendMessage(args);

            }
            else {
                ws->send("Unknown command", opCode);
            }
        },
        .close = [](auto* ws, int /*code*/, string_view /*msg*/) {
            auto* data = ws->getUserData();
            if (!data->username.empty()) {
                auto user = userMap[data->username];
                if (user) user->ws = nullptr;
                cout << data->username << " disconnected." << endl;
            }
            else {
                cout << "disconnected" << endl;
            }
        }
        }).listen(9001, [](auto* token) {
            if (token) {
                cout << "✅ Chat server started on port 9001" << endl;
            }
            else {
                cerr << "❌ Failed to start server!" << endl;
            }
            }).run();
}


/*

==============================
TESTING WEBSITE - https://piehost.com/websocket-tester
URL - ws://localhost:9001
==============================

==============================
📚 Chat Server Command Guide
==============================

✅ Account Commands
------------------------------

1. CREATE_USER:<username>,<password>
   → Creates a new user and logs them in automatically.
   Example:
       CREATE_USER:alice,1234
   Response:
       User created and logged in

2. LOGIN:<username>,<password>
   → Logs in an existing user.
   Example:
       LOGIN:alice,1234
   Response:
       Login successful


🏠 Room Commands
------------------------------

3. CREATE_ROOM:<room_name>
   → Creates a new room and adds the creator to it.
     A secret key (UUID) is generated and sent back.
   Example:
       CREATE_ROOM:myroom
   Response:
       Room created. Secret: 123e4567-e89b-12d3-a456-426614174000

4. JOIN_ROOM:<room_name>,<secret_key>
   → Joins an existing room using the correct secret key.
     If already joined before, it reconnects.
   Example:
       JOIN_ROOM:myroom,123e4567-e89b-12d3-a456-426614174000
   Responses:
       Joined room myroom
       OR
       Reconnected to room myroom
       OR
       Failed to join room (wrong key?)

5. CONNECT_ROOM:<room_name>
    → Connects to a room the user has joined before.
   Example:
       CONNECT_ROOM:myroom
   Response:
       Connected to room myroom
       OR
       You have not joined this room before. Use JOIN_ROOM with secret key.


💬 Messaging Command
------------------------------

6. SEND:<message>
   → Sends a message to all users in the current room
     except the sender.
   Example:
       SEND:Hello everyone!
   Other users receive:
       alice: Hello everyone!


🔌 Disconnection Behavior
------------------------------
- On disconnect, the user’s socket is cleared (ws = nullptr).
- Users are NOT removed from rooms or global maps.
- Server is long-running and maintains persistent state.

🧾 Global State Maintained
------------------------------
- Global sets for all unique usernames and room names.
- Global maps:
    - username → User class
    - room name → Room class
    - No active user sets (only per-user `ws` field)

*/