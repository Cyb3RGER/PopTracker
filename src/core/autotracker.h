#ifndef _CORE_AUTOTRACKER_H
#define _CORE_AUTOTRACKER_H

#include "../usb2snes/usb2snes.h"
#include "../uat/uatclient.h"
#include "signal.h"
#include <string>
#include <string.h>
#include <stdint.h>
#include <vector> // TODO: replace by uint8_t* ?
#include <list>
#include <set>
#include <chrono>
#include <thread>
#include "../luaglue/luainterface.h"
#include "../luaglue/lua_json.h"
#include "util.h"


// this class is designed to wrap multiple auto-tracker back-ends
class AutoTracker final : public LuaInterface<AutoTracker>{
    friend class LuaInterface;
    
public:
    AutoTracker(const std::string& platform, const std::set<std::string>& flags, const std::string& name="PopTracker")
    {
        if (strcasecmp(platform.c_str(), "snes")==0) {
            _snes = new USB2SNES(name);
        } else if (flags.find("uat") != flags.end()) {
            _uat = new UATClient();
            _uat->setInfoHandler([this](const UATClient::Info& info) {
                // TODO: let user select the slot and send sync after that
                _slot = info.slots.empty() ? "" : info.slots.front();
                if (!_slot.empty()) printf("slot selected: %s\n", sanitize_print(_slot).c_str());
                _uat->sync(_slot);
            });
            _uat->setVarHandler([this](const std::vector<UATClient::Var>& vars) {
                std::list<std::string> varNames;
                for (const auto& var : vars) {
                    if (var.slot != _slot) continue;
                    printf("%s:%s = %s\n",
                            sanitize_print(var.slot).c_str(),
                            sanitize_print(var.name).c_str(),
                            var.value.dump().c_str());
                    _vars[var.name] = var.value;
                    varNames.push_back(var.name);
                }
                // NOTE: for UAT we pass the event straight through
                onVariablesChanged.emit(this, varNames);
            });
        }
    }
    
    virtual ~AutoTracker()
    {
        bool spawnedWorkers = false;
        
        if (_snes) {
            if (_snes->mayBlockOnExit()) {
                // delete() may wait for socket timeout -> run destructor in another thread
                auto snes = _snes;
                std::thread([snes]() { delete snes; }).detach();
                spawnedWorkers = true;
            } else {
                delete _snes;
            }
        }
        _snes = nullptr;
        
        if (_uat) delete _uat;
        _uat = nullptr;

        if (spawnedWorkers) {
            // wait a bit if we started a thread to increase readability of logs
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        }
    }
    
    enum class State {
        Disconnected, 
        BridgeConnected,
        ConsoleConnected
    };
    
    Signal<State> onStateChange;
    Signal<> onDataChange;
    Signal<const std::list<std::string>&> onVariablesChanged;
    State getState() const { return _state; }
    
    bool doStuff()
    {
        // USB2SNES AUTOTRACKING
        if (_snes) _snes->connect();
        if (_snes && _snes->dostuff()) {
            State oldState = _state;
            bool wsConnected = _snes->wsConnected();
            bool snesConnected = wsConnected ? _snes->snesConnected() : false;
            
            if (snesConnected) {
                _state = State::ConsoleConnected;
            } else if (wsConnected) {
                _state = State::BridgeConnected;
            } else {
                _state = State::Disconnected;
            }
            
            if (_state != oldState) {
                onStateChange.emit(this, _state);
            } else if (_state == State::ConsoleConnected) {
                onDataChange.emit(this);
            }
            
            return true;
        }
        
        // UAT AUTOTRACKING
        if (_uat) _uat->connect();
        if (_uat && _uat->poll()) {
            State oldState = _state;
            _state = _uat->getState() == UATClient::State::GAME_CONNECTED ? State::ConsoleConnected :
                     _uat->getState() == UATClient::State::SOCKET_CONNECTED ? State::BridgeConnected :
                    State::Disconnected;
            if (_state != oldState) {
                onStateChange.emit(this, _state);
            }
            return true;
        }

        // NO UPDATES
        return false;
    }
    
    bool addWatch(unsigned addr, unsigned len)
    {
        if (len<0) return false;
        if (addr<=0xffffff && _snes) {
            _snes->addWatch((uint32_t)addr, len);
            return true;
        }
        return false;
    }
    bool removeWatch(unsigned addr, unsigned len)
    {
        if (len<0) return false;
        if (addr<=0xffffff && _snes) {
            _snes->removeWatch((uint32_t)addr, len);
            return true;
        }
        return false;
    }
    void setInterval(unsigned ms) {
        if (_snes)
            _snes->setUpdateInterval(ms);
    }
    void clearCache() {
        if (_snes)
            _snes->clearCache();
        if (_uat)
            _uat->sync(_slot);
    }
    
    // TODO: canRead(addr,len) to detect incomplete segment
    std::vector<uint8_t> read(unsigned addr, unsigned len)
    {
        std::vector<uint8_t> res;
        if (_snes) {
            uint8_t buf[len];
            _snes->read((uint32_t)addr, len, buf);
            for (size_t i=0; i<len; i++)
                res.push_back(buf[i]);
        }
        return res;
    }
    
    int ReadU8(int segment, int offset=0)
    {
        // NOTE: this is AutoTracker:Read8. we only have 1 segment, that is AutoTracker
        return ReadUInt8(segment+offset);
    }
    int ReadU16(int segment, int offset=0) { return ReadUInt16(segment+offset); }
    int ReadU24(int segment, int offset=0) { return ReadUInt24(segment+offset); }
    int ReadU32(int segment, int offset=0) { return ReadUInt32(segment+offset); }
    int ReadUInt8(int addr)
    {
        // NOTE: this is Segment:ReadUint8. we only have 1 segment, that is AutoTracker
        if (_snes) {
            auto res = _snes->read(addr);
            if (res == 0) _snes->addWatch(addr); // we don't read snes memory on the main thread.
            // TODO: canRead + maybe wait a little and try again
            //printf("$%06x = %02x\n", a, res);
            return res;
        }
        return 0;
    }
    int ReadUInt16(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint16_t>(addr);
            if (res == 0) _snes->addWatch(addr,2);
            return res;
        }
        return 0;
    }
    int ReadUInt24(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint32_t>(addr)&0xffffff;
            if (res == 0) _snes->addWatch(addr,3);
            return res;
        }
        return 0;
    }
    int ReadUInt32(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint32_t>(addr);
            if (res == 0) _snes->addWatch(addr,4);
            return res;
        }
        return 0;
    }
    json ReadVariable(const std::string& name)
    {
        auto it = _vars.find(name);
        if (it != _vars.end()) {
            return it->second;
        }
        return {};
    }
    
protected:
    
    State _state = State::Disconnected;
    USB2SNES *_snes = nullptr;
    UATClient *_uat = nullptr;
    std::string _slot; // selected slot for UAT
    std::map<std::string, nlohmann::json> _vars; // variable store for UAT
    
protected: // LUA interface implementation
    
    static constexpr const char Lua_Name[] = "AutoTracker";
    static const LuaInterface::MethodMap Lua_Methods;
};

#endif //_CORE_AUTOTRACKER_H
