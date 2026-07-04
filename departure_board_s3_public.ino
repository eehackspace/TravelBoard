/* ============================================================================
 *  HOCKLEY DEPARTURE BOARD  —  MatrixPortal S3 + 64x32 panel  (full build)
 *  Pages between three live feeds:
 *    TRAINS : Hockley (HOC) -> London Liverpool Street (LST), hiding any
 *             leaving within 10 min (time to reach the train)
 *    BUSES  : Arriva 7 from "The Victory" towards Rayleigh (runs via Greensward
 *             Lane / Hockley), hiding any leaving within 4 min (walk to stop)
 *    PLANE  : nearest aircraft overhead (shown only when one is in range)
 *
 *  LIBRARIES: Adafruit Protomatter, ArduinoJson (v7).
 *  Serial 115200.  Tools -> "USB CDC On Boot: Enabled".  Modest brightness (USB).
 *
 *  ---- ACCOUNTS / KEYS (free, personal use) — fill in the #defines below ----
 *    Realtime Trains : https://api-portal.rtt.io
 *       The portal issues a long-life REFRESH token (a long "ey..." JWT). The
 *       sketch auto-exchanges it for a short-life ACCESS token and refreshes as
 *       needed. Paste the refresh token into RTT_REFRESH_TOKEN below.
 *    TransportAPI    : https://developer.transportapi.com   (app_id + app_key)
 *       Free tier ~1000 req/day; bus polling at 120s ~= 720/day. Verify your cap.
 *    airplanes.live / adsbdb.com : no key needed.
 * ==========================================================================*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Protomatter.h>
#include <time.h>
#include <math.h>

// ===========================  CONFIG  ======================================
#define WIFI_SSID      "XXReplace with SSIDXX"
#define WIFI_PASSWORD  "XXReplace with your passwordXX"

// Trains (Realtime Trains — NEW API at data.rtt.io)
// The portal issues a long-life REFRESH token (the long "ey..." JWT). The sketch
// auto-exchanges it for a short-life ACCESS token via /api/get_access_token and
// refreshes when it expires — you only paste the refresh token here.
#define RTT_BASE_URL      "https://data.rtt.io"
#define RTT_REFRESH_TOKEN "XXPaste your token from the URL aboveXX"
#define TRAIN_ORIGIN   "HOC" //train origin
#define TRAIN_DEST     "LST" //train destintion
#define TRAIN_LEAD_MIN 10 //time it takes me to drive to the station (you can have this as 0)

// Buses (TransportAPI)
#define TAPI_APP_ID    "XXAppIDXX"
#define TAPI_APP_KEY   "XXAppKeyXX"
#define BUS_STOP_ATCO  "XX11 digit ID numberXX"      // verified ATCO ID
#define BUS_LINE       "7"  //I use the number 7 bus only
#define BUS_DEST_MATCH "Rayleigh"          // 7-to-Rayleigh runs via Greensward/Hockley
#define BUS_LEAD_MIN   4 //Takes me 4 minutes to get to my bus stop by foot!

// Plane (airplanes.live, no key)
#define LAT      51.500000  //replace with your own Lat/Long
#define LON      0.60000
#define LAT_STR  "51.500000"
#define LON_STR  "0.60000"  
#define RADIUS_NM 15 //now many nautical miles you want to scan - keep it relatively small 5-15 nm is fine.

// Polling + paging
#define TRAIN_POLL_MS 60000UL
#define BUS_POLL_MS   3600000UL   // 1 hour! TransportAPI free tier = only 30 hits/day,
                                  // so ~24/day at hourly. Lower this only on a paid plan.
#define PLANE_POLL_MS 10000UL
#define PAGE_DWELL_MS 5000UL
#define FRAME_MS      60UL
#define MAX_TRAINS 2
#define MAX_BUSES  2

// UK timezone (DST-aware)
#define TZ_UK "GMT0BST,M3.5.0/1,M10.5.0/2"

// ===========================  DISPLAY (MatrixPortal S3 pins)  ===============
uint8_t rgbPins[]  = {42, 41, 40, 38, 39, 37};
uint8_t addrPins[] = {45, 36, 48, 35, 21};
uint8_t clockPin   = 2;
uint8_t latchPin   = 47;
uint8_t oePin      = 14;
Adafruit_Protomatter matrix(64, 4, 1, rgbPins, 4, addrPins,
                            clockPin, latchPin, oePin, false);

uint16_t C_CYAN, C_WHITE, C_GREEN, C_AMBER, C_RED, C_GREY;

// Protomatter has no setBrightness() — brightness is scaled into the colours.
// Change this one number (0.0–1.0). Lower = dimmer and less current draw.
#define BRIGHTNESS 0.5
uint16_t dim(uint8_t r, uint8_t g, uint8_t b){
  return matrix.color565((uint8_t)(r*BRIGHTNESS),(uint8_t)(g*BRIGHTNESS),(uint8_t)(b*BRIGHTNESS));
}

// ===========================  STATE  =======================================
struct TrainItem { char tstr[6]; int mins; char plat[4]; bool cancelled; int delay; };
struct BusItem   { char tstr[6]; int mins; bool realtime; };
struct PlaneItem { bool present; char callsign[12]; char type[8]; char route[16]; long altFt; float distNm; int bearing; };

TrainItem g_trains[MAX_TRAINS]; int g_trainCount=0; bool g_trainsValid=false;
BusItem   g_buses[MAX_BUSES];   int g_busCount=0;   bool g_busesValid=false;
PlaneItem g_plane = {false,"","","",0,0,0};

// ===========================  TIME  ========================================
static long daysFromCivil(int y,int m,int d);    // (defined below)

// Weekday for a civil date: 0=Sun .. 6=Sat
static int weekdayOf(int y,int m,int d){ return (int)(((daysFromCivil(y,m,d)%7)+4+7)%7); }
// Day-of-month of the last Sunday in (y,m)
static int lastSundayOfMonth(int y,int m){
  static const int dim[]={31,28,31,30,31,30,31,31,30,31,30,31};
  int days=dim[m-1];
  if(m==2 && ((y%4==0&&y%100!=0)||y%400==0)) days=29;
  return days - weekdayOf(y,m,days);
}
// Is this UTC broken-down time within UK BST? (BST = last Sun Mar 01:00 UTC .. last Sun Oct 01:00 UTC)
static bool ukIsBST(const struct tm* g){
  int y=g->tm_year+1900, mo=g->tm_mon+1, d=g->tm_mday, h=g->tm_hour;
  if(mo<3||mo>10) return false;
  if(mo>3&&mo<10) return true;
  int ls=lastSundayOfMonth(y,mo);
  if(mo==3) return (d>ls)||(d==ls&&h>=1);
  return (d<ls)||(d==ls&&h<1);
}
// UK local time as minutes-since-midnight, derived straight from the synced UTC clock —
// independent of the device's timezone setting (which wasn't applying reliably).
int ukNowMinutes(){
  time_t now=time(nullptr);
  if(now<100000) return -1;                   // clock not synced yet
  struct tm g; gmtime_r(&now,&g);
  int mins=g.tm_hour*60+g.tm_min+(ukIsBST(&g)?60:0);
  return mins%1440;
}
int minsUntil(int hh,int mm){
  int now=ukNowMinutes();
  if(now<0) return -9999;
  int d=(hh*60+mm)-now;
  if(d<-120) d+=1440;
  return d;
}
int parseHHMM(const char* s,char* out){ (void)s; (void)out; return -9999; } // (unused, kept for compat)

// --- ISO-8601 helpers for the new RTT API ("2025-10-25T08:36:00+01:00" or "...Z") ---
// days since 1970-01-01 for a civil (y,m,d) date — Howard Hinnant's algorithm
static long daysFromCivil(int y,int m,int d){
  y -= m <= 2;
  long era = (y>=0 ? y : y-399)/400;
  unsigned yoe = (unsigned)(y - era*400);
  unsigned doy = (153*(m + (m>2 ? -3 : 9)) + 2)/5 + d - 1;
  unsigned doe = yoe*365 + yoe/4 - yoe/100 + doy;
  return era*146097L + (long)doe - 719468L;
}
// Parse ISO-8601 ("...T08:36:00+01:00" or "...Z") to a UTC epoch (no timegm needed).
time_t parseISOtoEpoch(const char* s){
  if(!s||strlen(s)<19||s[10]!='T') return 0;
  int Y =atoi(s);
  int Mo=(s[5]-'0')*10+(s[6]-'0');
  int D =(s[8]-'0')*10+(s[9]-'0');
  int h =(s[11]-'0')*10+(s[12]-'0');
  int mi=(s[14]-'0')*10+(s[15]-'0');
  int se=(s[17]-'0')*10+(s[18]-'0');
  time_t e=(time_t)(daysFromCivil(Y,Mo,D)*86400L + h*3600L + mi*60L + se);
  char sign=s[19];                            // apply any +hh:mm / -hh:mm offset
  if(sign=='+'||sign=='-'){
    int oh=(s[20]-'0')*10+(s[21]-'0');
    int om=(s[23]-'0')*10+(s[24]-'0');
    int off=(oh*60+om)*60;
    e += (sign=='+') ? -off : off;
  }
  return e;
}
int minsUntilEpoch(time_t when){
  time_t now=time(nullptr);
  if(when==0||now<100000) return -9999;
  return (int)((when-now)/60);
}
void epochToLocalHHMM(time_t t,char* out){   // local UK wall-clock for display
  struct tm lt; localtime_r(&t,&lt);
  snprintf(out,6,"%02d:%02d",lt.tm_hour,lt.tm_min);
}
int parseHColon(const char* s,char* out){          // "08:36"
  if(!s||strlen(s)<4){ out[0]=0; return -9999; }
  int hh=0,mm=0; if(sscanf(s,"%d:%d",&hh,&mm)!=2){ out[0]=0; return -9999; }
  snprintf(out,6,"%02d:%02d",hh,mm); return minsUntil(hh,mm);
}
// RTT departure times are UK LOCAL wall-clock ("2025-06-26T11:42:00..."). Read the
// HH:MM straight off (no UTC round-trip) — that's the displayed time, and minsUntil
// compares it to local now. Avoids double-applying the BST offset.
int parseRttTime(const char* s,char* out){
  if(!s||strlen(s)<16||s[10]!='T'){ out[0]=0; return -9999; }
  int hh=(s[11]-'0')*10+(s[12]-'0');
  int mm=(s[14]-'0')*10+(s[15]-'0');
  snprintf(out,6,"%02d:%02d",hh,mm); return minsUntil(hh,mm);
}

// ===========================  GEO  =========================================
double toRad(double d){ return d*M_PI/180.0; }
double haversineNm(double a1,double o1,double a2,double o2){
  const double R=3440.065; double dA=toRad(a2-a1),dO=toRad(o2-o1);
  double x=sin(dA/2)*sin(dA/2)+cos(toRad(a1))*cos(toRad(a2))*sin(dO/2)*sin(dO/2);
  return R*2*atan2(sqrt(x),sqrt(1-x));
}
int bearingDeg(double a1,double o1,double a2,double o2){
  double dO=toRad(o2-o1); double y=sin(dO)*cos(toRad(a2));
  double x=cos(toRad(a1))*sin(toRad(a2))-sin(toRad(a1))*cos(toRad(a2))*cos(dO);
  return ((int)round(atan2(y,x)*180.0/M_PI)+360)%360;
}

// ===========================  HTTP  ========================================
bool httpGetJson(const char* url,JsonDocument& doc,JsonDocument& filter,
                 const char* user=nullptr,const char* pass=nullptr){
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setConnectTimeout(6000); http.setTimeout(9000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);   // follow 301/302
  if(!http.begin(client,url)) return false;
  if(user) http.setAuthorization(user,pass);
  http.addHeader("Accept","application/json");
  http.addHeader("User-Agent","esp32-departure-board");
  int code=http.GET();
  if(code!=200){ Serial.printf("[HTTP] %d %s\n",code,url); http.end(); return false; }
  String body=http.getString();                            // read fully (more reliable than stream)
  http.end();
  DeserializationError e=deserializeJson(doc,body,DeserializationOption::Filter(filter));
  if(e){ Serial.printf("[JSON] %s\n",e.c_str()); return false; }
  return true;
}

// ===========================  FEED: TRAINS  ================================
// The portal token is a REFRESH token; exchange it for a short-life ACCESS token
// via GET /api/get_access_token (Bearer = refresh token), then use the access
// token for queries. Re-exchange shortly before it expires.
char   g_rttAccess[1100] = "";
time_t g_rttAccessExp = 0;

bool refreshRttAccess(){
  char url[96]; snprintf(url,sizeof(url),"%s/api/get_access_token",RTT_BASE_URL);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setConnectTimeout(6000); http.setTimeout(9000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if(!http.begin(client,url)) return false;
  char auth[1100]; snprintf(auth,sizeof(auth),"Bearer %s",RTT_REFRESH_TOKEN);
  http.addHeader("Authorization",auth);
  http.addHeader("Accept","application/json");
  int code=http.GET();
  String body=http.getString();
  http.end();
  if(code!=200){ Serial.printf("[RTT token] HTTP %d: %s\n",code,body.substring(0,180).c_str()); return false; }
  JsonDocument doc;
  DeserializationError e=deserializeJson(doc,body);
  if(e){ Serial.printf("[RTT token] JSON %s; body: %s\n",e.c_str(),body.substring(0,180).c_str()); return false; }
  const char* tok=doc["token"]; const char* vu=doc["validUntil"];
  if(!tok){ Serial.printf("[RTT token] no token; body: %s\n",body.substring(0,180).c_str()); return false; }
  strncpy(g_rttAccess,tok,sizeof(g_rttAccess)-1); g_rttAccess[sizeof(g_rttAccess)-1]=0;
  g_rttAccessExp = vu ? parseISOtoEpoch(vu) : (time(nullptr)+1800);
  Serial.printf("[RTT token] ok (valid until %s)\n", vu?vu:"?");
  return true;
}

void fetchTrains(){
  Serial.printf("[clock] UK %02d:%02d\n", ukNowMinutes()/60, ukNowMinutes()%60);
  // ensure a current access token (refresh ~60s before expiry)
  if(!g_rttAccess[0] || time(nullptr) > g_rttAccessExp-60){
    if(!refreshRttAccess()) return;
  }

  char url[160];
  snprintf(url,sizeof(url),"%s/gb-nr/location?code=%s&filterTo=%s",
           RTT_BASE_URL,TRAIN_ORIGIN,TRAIN_DEST);

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.setConnectTimeout(6000); http.setTimeout(9000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if(!http.begin(client,url)) return;
  char auth[1100]; snprintf(auth,sizeof(auth),"Bearer %s",g_rttAccess);
  http.addHeader("Authorization",auth);
  http.addHeader("Accept","application/json");

  int code=http.GET();
  if(code==401){ Serial.println("[TRAINS] 401 — refreshing token"); g_rttAccessExp=0; http.end(); return; }
  if(code==204){ g_trainCount=0; g_trainsValid=true; http.end(); Serial.println("[TRAINS] 0"); return; }
  if(code!=200){ Serial.printf("[TRAINS] HTTP %d\n",code); http.end(); return; }
  String body=http.getString();
  http.end();

  JsonDocument filter;
  filter["services"][0]["temporalData"]["departure"]["scheduleAdvertised"]=true;
  filter["services"][0]["temporalData"]["departure"]["realtimeForecast"]=true;
  filter["services"][0]["temporalData"]["departure"]["realtimeAdvertisedLateness"]=true;
  filter["services"][0]["temporalData"]["departure"]["isCancelled"]=true;
  filter["services"][0]["temporalData"]["displayAs"]=true;
  filter["services"][0]["locationMetadata"]["platform"]["actual"]=true;
  filter["services"][0]["locationMetadata"]["platform"]["planned"]=true;

  JsonDocument doc;
  DeserializationError e=deserializeJson(doc,body,DeserializationOption::Filter(filter));
  if(e){ Serial.printf("[TRAINS] JSON %s\n",e.c_str()); return; }

  int n=0;
  for(JsonObject svc : doc["services"].as<JsonArray>()){
    JsonObject dep=svc["temporalData"]["departure"];
    const char* sched=dep["scheduleAdvertised"];
    const char* fore =dep["realtimeForecast"];
    const char* disp =svc["temporalData"]["displayAs"];
    const char* useT =fore?fore:sched;
    if(!useT) continue;
    char tstr[6]; int mins=parseRttTime(useT,tstr);
    if(mins<TRAIN_LEAD_MIN||mins<=-9999) continue;
    bool canc=(dep["isCancelled"]|false)||(disp&&!strcmp(disp,"CANCELLED"));
    int delay=dep["realtimeAdvertisedLateness"]|0; if(delay<0)delay=0;
    const char* plat=svc["locationMetadata"]["platform"]["actual"]
                    |svc["locationMetadata"]["platform"]["planned"]|"-";
    strncpy(g_trains[n].tstr,tstr,6); g_trains[n].mins=mins;
    strncpy(g_trains[n].plat,plat,4); g_trains[n].plat[3]=0;
    g_trains[n].cancelled=canc; g_trains[n].delay=delay;
    if(++n>=MAX_TRAINS) break;
  }
  g_trainCount=n; g_trainsValid=true;
  Serial.printf("[TRAINS] %d\n",n);
}

// ===========================  FEED: BUSES  ================================
void fetchBuses(){
  char url[256];
  // nextbuses=no -> scheduled times only, but counts as 1 hit (nextbuses=yes is
  // "expensive" and burns the 30/day free quota much faster). Route 7's timetable
  // is regular, so scheduled is fine. Switch to yes only on a paid plan.
  snprintf(url,sizeof(url),
    "https://transportapi.com/v3/uk/bus/stop/%s/live.json?app_id=%s&app_key=%s&group=route&nextbuses=no",
    BUS_STOP_ATCO,TAPI_APP_ID,TAPI_APP_KEY);
  JsonDocument filter; filter["departures"]=true;
  JsonDocument doc;
  if(!httpGetJson(url,doc,filter)) return;

  BusItem tmp[12]; int n=0;
  for(JsonPair kv : doc["departures"].as<JsonObject>()){
    for(JsonObject d : kv.value().as<JsonArray>()){
      const char* line=d["line"]|d["line_name"]|"";
      const char* dir =d["direction"]|"";
      if(strcmp(line,BUS_LINE)) continue;
      if(!strstr(dir,BUS_DEST_MATCH)) continue;
      const char* best=d["best_departure_estimate"]|d["expected_departure_time"]|d["aimed_departure_time"]|(const char*)nullptr;
      bool rt=d["expected_departure_time"].is<const char*>()&&d["expected_departure_time"];
      if(!best) continue;
      char tstr[6]; int mins=parseHColon(best,tstr);
      if(mins<BUS_LEAD_MIN||mins<=-9999) continue;
      if(n<12){ strncpy(tmp[n].tstr,tstr,6); tmp[n].mins=mins; tmp[n].realtime=rt; n++; }
    }
  }
  for(int i=1;i<n;i++){ BusItem k=tmp[i]; int j=i-1; while(j>=0&&tmp[j].mins>k.mins){tmp[j+1]=tmp[j];j--;} tmp[j+1]=k; }
  int keep=n<MAX_BUSES?n:MAX_BUSES;
  for(int i=0;i<keep;i++) g_buses[i]=tmp[i];
  g_busCount=keep; g_busesValid=true;
  Serial.printf("[BUSES] %d\n",keep);
}

// ===========================  FEED: PLANE  ================================
void lookupRoute(const char* cs,char* out,size_t n){
  out[0]=0; if(!cs||!cs[0]) return;
  char url[96]; snprintf(url,sizeof(url),"https://api.adsbdb.com/v0/callsign/%s",cs);
  JsonDocument filter;
  filter["response"]["flightroute"]["origin"]["iata_code"]=true;
  filter["response"]["flightroute"]["destination"]["iata_code"]=true;
  JsonDocument doc; if(!httpGetJson(url,doc,filter)) return;
  const char* o=doc["response"]["flightroute"]["origin"]["iata_code"];
  const char* d=doc["response"]["flightroute"]["destination"]["iata_code"];
  if(o&&d) snprintf(out,n,"%s>%s",o,d);
}
void fetchPlane(){
  char url[128]; snprintf(url,sizeof(url),"https://api.airplanes.live/v2/point/%s/%s/%d",LAT_STR,LON_STR,RADIUS_NM);
  JsonDocument filter;
  filter["ac"][0]["flight"]=true; filter["ac"][0]["r"]=true; filter["ac"][0]["t"]=true;
  filter["ac"][0]["alt_baro"]=true; filter["ac"][0]["lat"]=true; filter["ac"][0]["lon"]=true;
  JsonDocument doc; PlaneItem p={false,"","","",0,0,0};
  if(httpGetJson(url,doc,filter)){
    double best=1e9; JsonObject bestAc;
    for(JsonObject ac : doc["ac"].as<JsonArray>()){
      if(!ac["lat"].is<double>()||!ac["lon"].is<double>()) continue;
      double dd=haversineNm(LAT,LON,ac["lat"].as<double>(),ac["lon"].as<double>());
      if(dd<best){ best=dd; bestAc=ac; }
    }
    if(!bestAc.isNull()){
      p.present=true;
      const char* fl=bestAc["flight"]|""; const char* rg=bestAc["r"]|"";
      char cs[12]; strncpy(cs,fl[0]?fl:rg,11); cs[11]=0;
      for(int i=strlen(cs)-1;i>=0&&cs[i]==' ';i--) cs[i]=0;
      strncpy(p.callsign,cs,12);
      strncpy(p.type,(const char*)(bestAc["t"]|""),7); p.type[7]=0;
      if(bestAc["alt_baro"].is<long>()) p.altFt=bestAc["alt_baro"].as<long>();
      p.distNm=(float)best; p.bearing=bearingDeg(LAT,LON,bestAc["lat"].as<double>(),bestAc["lon"].as<double>());
      // Only airline callsigns (contain a digit, e.g. BAW283) have routes. Registrations
      // (GBBBN, GTOLL...) never do, so skip the lookup — saves a guaranteed 404 each time.
      bool hasDigit=false; for(const char* c=p.callsign;*c;c++) if(*c>='0'&&*c<='9'){hasDigit=true;break;}
      if(hasDigit) lookupRoute(p.callsign,p.route,sizeof(p.route));
    }
  }
  g_plane=p;
  Serial.printf("[PLANE] %s\n",p.present?p.callsign:"(none)");
}

// ===========================  DRAW  ========================================
void drawCentered(int y,const char* s,uint16_t c){
  int x=(64-(int)strlen(s)*6)/2; if(x<0)x=0;
  matrix.setTextColor(c); matrix.setCursor(x,y); matrix.print(s);
}
void drawLeft(int y,const char* s,uint16_t c){
  matrix.setTextColor(c); matrix.setCursor(0,y); matrix.print(s);
}

void renderTrains(){
  matrix.fillScreen(0); drawCentered(0,"HOC>LST",C_CYAN);
  if(!g_trainsValid){ drawCentered(12,"loading",C_GREY); return; }
  if(g_trainCount==0){ drawCentered(12,"no trains",C_GREY); return; }
  for(int i=0;i<g_trainCount;i++){
    char b[20]; int y=9+i*10;
    if(g_trains[i].cancelled){ snprintf(b,sizeof(b),"%s CANC",g_trains[i].tstr); drawLeft(y,b,C_RED); }
    else { snprintf(b,sizeof(b),"%s %s %dm",g_trains[i].tstr,g_trains[i].plat,g_trains[i].mins);
           drawLeft(y,b,g_trains[i].delay>0?C_RED:C_AMBER); }
  }
}
void renderBuses(){
  matrix.fillScreen(0); drawCentered(0,"BUS7>RAY",C_CYAN);
  if(!g_busesValid){ drawCentered(12,"loading",C_GREY); return; }
  if(g_busCount==0){ drawCentered(12,"no buses",C_GREY); return; }
  for(int i=0;i<g_busCount;i++){
    char b[20]; int y=9+i*10;
    snprintf(b,sizeof(b),"%s %dm%s",g_buses[i].tstr,g_buses[i].mins,g_buses[i].realtime?"*":"");
    drawLeft(y,b,C_AMBER);
  }
}
void renderPlane(){
  matrix.fillScreen(0); drawCentered(0,"OVERHEAD",C_CYAN);
  if(!g_plane.present){ drawCentered(12,"clear",C_GREY); drawCentered(20,"skies",C_GREY); return; }
  drawCentered(9,g_plane.callsign,C_WHITE);
  // route (airliners) in green, else aircraft type (GA) in green
  const char* mid = g_plane.route[0] ? g_plane.route : g_plane.type;
  if(mid[0]) drawCentered(17,mid,C_GREEN);
  char info[20]; long kft=(g_plane.altFt+500)/1000;
  snprintf(info,sizeof(info),"%.0fnm %ldk",g_plane.distNm,kft);
  drawCentered(mid[0]?25:18,info,C_AMBER);
}

// ===========================  SETUP / LOOP  ================================
void connectWiFi(){
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID,WIFI_PASSWORD);
  unsigned long t0=millis(); Serial.print("[wifi]");
  while(WiFi.status()!=WL_CONNECTED&&millis()-t0<20000){ delay(300); Serial.print('.'); }
  Serial.println(WiFi.status()==WL_CONNECTED?" ok":" FAILED");
}

void setup(){
  Serial.begin(115200); delay(1200);
  Serial.println("\n=== Hockley Departure Board ===");

  ProtomatterStatus s=matrix.begin();
  if(s!=PROTOMATTER_OK){ Serial.printf("Protomatter init failed: %d\n",s); while(1) delay(10); }
  matrix.setTextSize(1); matrix.setTextWrap(false);
  C_CYAN=dim(0,180,200); C_WHITE=dim(255,255,255); C_GREEN=dim(0,200,80);
  C_AMBER=dim(255,170,0); C_RED=dim(255,60,40); C_GREY=dim(110,110,110);

  matrix.fillScreen(0); drawCentered(12,"wifi...",C_AMBER); matrix.show();
  connectWiFi();
  if(WiFi.status()==WL_CONNECTED) Serial.printf("IP %s\n",WiFi.localIP().toString().c_str());

  configTzTime(TZ_UK,"pool.ntp.org","time.nist.gov");
  struct tm ti; for(int i=0;i<20&&!getLocalTime(&ti,200);i++) delay(200);

  matrix.fillScreen(0); drawCentered(12,"loading",C_AMBER); matrix.show();
  if(WiFi.status()==WL_CONNECTED){ fetchTrains(); fetchBuses(); fetchPlane(); }
}

void loop(){
  static unsigned long lastTrain=0,lastBus=0,lastPlane=0,lastPage=0,lastFrame=0;
  static int page=0;   // 0 trains, 1 buses, 2 plane

  // WiFi watchdog
  if(WiFi.status()!=WL_CONNECTED){
    static unsigned long lastTry=0;
    if(millis()-lastTry>10000){ lastTry=millis(); WiFi.reconnect(); }
  }

  // staggered fetches — at most one per pass so they never stack
  unsigned long now=millis();
  if(WiFi.status()==WL_CONNECTED){
    if(now-lastPlane>=PLANE_POLL_MS){ lastPlane=now; fetchPlane(); }
    else if(now-lastTrain>=TRAIN_POLL_MS){ lastTrain=now; fetchTrains(); }
    else if(now-lastBus>=BUS_POLL_MS){ lastBus=now; fetchBuses(); }
  }

  // page rotation (skip plane page when nothing overhead)
  if(now-lastPage>=PAGE_DWELL_MS){
    lastPage=now;
    for(int g=0;g<3;g++){ page=(page+1)%3; if(page!=2||g_plane.present) break; }
  }

  // render
  if(now-lastFrame>=FRAME_MS){
    lastFrame=now;
    switch(page){ case 0: renderTrains(); break; case 1: renderBuses(); break; case 2: renderPlane(); break; }
    matrix.show();
  }
}
