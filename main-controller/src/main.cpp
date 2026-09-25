#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_system.h>
#include <BoardClient.h>
#include <WifiOta.h>
#include "board_config.h"
#include "ota_identity.h"

extern const uint8_t indexStart[] asm("_binary_data_index_html_start");
extern const uint8_t indexEnd[] asm("_binary_data_index_html_end");
using namespace babytech::v2;
namespace {
HardwareSerial deviceLink(1);
WebServer server(80);
Parser parser;
babytech::v2::Client client;
uint32_t lastByteAt=0;
bool apStarted=false;
bool safeForOta() {
    return client.connected(millis()) && !client.controlBusy() &&
        client.status().state == State::Idle &&
        (client.motorFlags(millis()) & (8 | 16 | 32)) == 0;
}
bool otaHealthy() { return apStarted && WiFi.softAPIP() != IPAddress(0,0,0,0); }
babytech::WifiOta ota(server,BABYTECH_OTA_BOARD,BABYTECH_OTA_HARDWARE,
                      BABYTECH_OTA_VERSION,BABYTECH_OTA_BUILD,BABYTECH_OTA_IMAGE_ID,
                      safeForOta,otaHealthy);

String bootText() {
    char text[17]; snprintf(text,sizeof(text),"%016llx",(unsigned long long)client.boot()); return String(text);
}
void sendJson(int code,const String& body) {
    server.sendHeader("Cache-Control","no-store"); server.send(code,"application/json",body);
}
void sendFrame(const Frame& f) {
    uint8_t bytes[kMaxFrameSize]; const size_t n=encode(f,bytes,sizeof(bytes));
    if (n) deviceLink.write(bytes,n);
}
void queryDevice(bool force=false) {
    Frame f; if (client.query(millis(),f,force)) sendFrame(f);
}
void pollDevice() {
    if (millis()-lastByteAt>kByteTimeoutMs) parser.reset();
    for (size_t n=0;n<256 && deviceLink.available()>0;++n) {
        lastByteAt=millis(); Frame f;
        if (parser.push(uint8_t(deviceLink.read()),f)) client.receive(f,millis());
    }
}
const char* outcomeText() {
    if (!client.haveControl()) return "none";
    if (!client.controlReplied() && client.controlReason()==Reason::BootMismatch) return "unknown_after_restart";
    if (client.controlUnknown(millis())) return "unknown";
    if (!client.controlReplied()) return "waiting";
    switch(client.controlOutcome()) {
        case Outcome::Ok:return "ok"; case Outcome::Accepted:return "accepted";
        case Outcome::Done:return "done"; case Outcome::Rejected:return "rejected";
        case Outcome::Failed:return "failed"; case Outcome::Cancelled:return "cancelled";
    }
    return "unknown";
}
void sendStatus() {
    const auto& s=client.status(); const auto& p=client.parameters();
    const bool online=client.connected(millis());
    static const char* states[]={"not_configured","idle","running","stopping","fault"};
    String body="{\"connected\":"; body+=online?"true":"false";
    body+=",\"protocolVersion\":2,\"boot\":\""; body+=bootText();
    body+="\",\"motionState\":\""; body+=online?states[uint8_t(s.state)]:"offline";
    body+="\",\"motionUptimeMs\":"; body+=online?String(s.uptime):String("null");
    body+=",\"responses\":"; body+=client.responses();
    body+=",\"motorsAvailable\":"; body+=online&&s.motors?"true":"false";
    body+=",\"sensorsAvailable\":false,\"revision\":"; body+=s.revision;
    body+=",\"parameterRevision\":"; body+=client.parameterRevision();
    body+=",\"motorFlags\":"; body+=online?String(client.motorFlags(millis())):String("null");
    body+=",\"busy\":"; body+=client.controlBusy()?"true":"false";
    body+=",\"command\":\""; body+=outcomeText();
    body+="\",\"commandSequence\":"; body+=client.controlSequence();
    body+=",\"reason\":"; body+=uint16_t(client.controlReason());
    body+=",\"params\":{\"motor\":"; body+=p.motor;
    body+=",\"angleTenths\":"; body+=p.angle;
    body+=",\"speedTenths\":"; body+=p.speed;
    body+=",\"accel\":"; body+=p.accel; body+=",\"decel\":"; body+=p.decel;
    body+=",\"current\":"; body+=p.current; body+="}}"; sendJson(200,body);
}
bool integerArg(const char* key,int64_t min,int64_t max,int64_t& out) {
    if (!server.hasArg(key)) return false;
    const String raw=server.arg(key); size_t i=0; bool negative=false;
    if (!raw.length() || raw.length()>11) return false;
    if (raw[0]=='-') { negative=true; i=1; }
    if (i==raw.length()) return false;
    int64_t value=0;
    for (;i<raw.length();++i) { if (raw[i]<'0'||raw[i]>'9') return false; value=value*10+raw[i]-'0'; }
    out=negative?-value:value; return out>=min && out<=max;
}
bool expectedRevision(uint32_t& revision) {
    int64_t value;
    if (!client.connected(millis()) || server.arg("boot")!=bootText() ||
        !integerArg("revision",1,UINT32_MAX,value)) {
        sendJson(409,"{\"error\":\"refresh_status_and_parameters\"}"); return false;
    }
    revision=uint32_t(value); return true;
}
void dispatch(Frame& f) {
    if (!client.submit(f,millis())) { sendJson(409,"{\"error\":\"offline_or_command_unresolved\"}"); return; }
    sendFrame(f); String body="{\"queued\":true,\"sequence\":"; body+=f.sequence; body+="}";
    sendJson(202,body);
}
void writeParameters() {
    if (ota.maintenanceActive()) { sendJson(409,"{\"error\":\"ota_active\"}"); return; }
    uint32_t revision; if (!expectedRevision(revision)) return;
    static const char* names[]={"motor","angleTenths","speedTenths","accel","decel","current"};
    static const int64_t minimum[]={1,-36000,1,1,1,100},maximum[]={255,36000,1200,240,240,5000};
    Parameters p;
    for (uint16_t i=0;i<6;++i) {
        int64_t value;
        if (!integerArg(names[i],minimum[i],maximum[i],value)) { sendJson(400,"{\"error\":\"invalid_parameter\"}"); return; }
        p.set(i+1,uint32_t(value));
    }
    if (!validParameters(p)) { sendJson(400,"{\"error\":\"invalid_stage_or_duration_over_60s\"}"); return; }
    Frame f; f.cmd=Cmd::Write; Writer w(f.payload,kMaxPayload);
    w.put(client.boot(),8); w.put(revision,4); w.put(6,1);
    for (uint16_t field=1;field<=6;++field) {
        w.put(kStage,1); w.put(kMoveStage,2); w.put(field,2); w.put(4,2); w.put(p.get(field),4);
    }
    f.length=w.size(); dispatch(f);
}
void execute() {
    if (ota.maintenanceActive()) { sendJson(409,"{\"error\":\"ota_active\"}"); return; }
    uint32_t revision; if (!expectedRevision(revision)) return;
    const String action=server.arg("action");
    uint8_t cls=kStage; uint16_t instance=kMoveStage,op=kRun;
    if (action=="enable" || action=="disable") {
        int64_t id; if (!integerArg("motor",1,255,id)) { sendJson(400,"{\"error\":\"invalid_motor\"}"); return; }
        cls=kMotor; instance=uint16_t(id); op=action=="enable"?kEnable:kDisable;
    } else if (action!="run") { sendJson(400,"{\"error\":\"unsupported_action\"}"); return; }
    Frame f; f.cmd=Cmd::Exec; Writer w(f.payload,kMaxPayload);
    w.put(client.boot(),8); w.put(cls,1); w.put(instance,2); w.put(op,2); w.put(revision,4);
    f.length=w.size(); dispatch(f);
}
void stopAll() {
    Frame f; f.cmd=Cmd::Stop; Writer w(f.payload,kMaxPayload);
    w.put(0,8); f.length=w.size(); dispatch(f);
}
}
void setup() {
    Serial.begin(115200);
    client.begin((uint64_t(esp_random())<<32)|esp_random());
    deviceLink.begin(kLinkBaud,SERIAL_8N1,kLinkRxPin,kLinkTxPin);
    WiFi.mode(WIFI_AP);
    apStarted=WiFi.softAP("Babytech-Debug","babytech-demo");
    ota.begin();
    server.on("/",HTTP_GET,[] {
        server.send_P(200,"text/html; charset=utf-8",reinterpret_cast<const char*>(indexStart),indexEnd-indexStart);
    });
    server.on("/api/status",HTTP_GET,sendStatus);
    server.on("/api/query",HTTP_POST,[] { queryDevice(true); sendJson(202,"{\"queued\":true}"); });
    server.on("/api/params",HTTP_POST,writeParameters);
    server.on("/api/exec",HTTP_POST,execute);
    server.on("/api/stop",HTTP_POST,stopAll);
    server.onNotFound([] { sendJson(404,"{\"error\":\"not_found\"}"); });
    server.begin(); queryDevice(true);
}
void loop() {
    pollDevice(); queryDevice(); server.handleClient(); ota.poll(); delay(1);
}
