#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

class SimpleWiFiManager
{
private:
    WebServer server;
    DNSServer dnsServer;
    bool serverStarted = false;
    bool configComplete = false;
    const int CONFIG_TIMEOUT = 300000; // 5-minute timeout
    unsigned long configStartTime;
    const byte DNS_PORT = 53;

    // Configuration file path
    const char *CONFIG_FILE = "/wifi_config.json";

    struct Config
    {
        char ssid[33];
        char password[65];
        char param1[129]; // URL
        char param2[33];  // Additional parameter
        bool configured;
    } config;

    // Function to dynamically generate configuration page
    String getConfigPage()
    {
        String page = R"(
            <!DOCTYPE html>
            <html>
            <head>
                <title>WiFi Setup</title>
                <meta charset="UTF-8">
                <meta name="viewport" content="width=device-width, initial-scale=1">
                <style>
                    body { 
                        font-family: Arial; 
                        margin: 20px;
                        background-color: #f0f0f0;
                    }
                    .container {
                        max-width: 400px;
                        margin: 0 auto;
                        background: white;
                        padding: 20px;
                        border-radius: 8px;
                        box-shadow: 0 2px 4px rgba(0,0,0,0.1);
                    }
                    input {
                        margin: 10px 0;
                        padding: 8px;
                        width: 100%;
                        border: 1px solid #ddd;
                        border-radius: 4px;
                        box-sizing: border-box;
                    }
                    button {
                        padding: 10px 20px;
                        background-color: #007bff;
                        color: white;
                        border: none;
                        border-radius: 4px;
                        cursor: pointer;
                        width: 100%;
                    }
                    button:hover {
                        background-color: #0056b3;
                    }
                    .field {
                        margin-bottom: 15px;
                    }
                    label {
                        display: block;
                        margin-bottom: 5px;
                        color: #333;
                    }
                    .status {
                        margin-bottom: 20px;
                        padding: 10px;
                        background-color: #e8f5e9;
                        border-radius: 4px;
                        border-left: 4px solid #4caf50;
                    }
                </style>
            </head>
            <body>
                <div class="container">
                    <h1>WiFi Setup</h1>
        )";

        // If already configured, display current status
        if (config.configured) {
            page += "<div class='status'>";
            page += "<p><strong>Current Configuration:</strong></p>";
            page += "<p>Connected SSID: " + String(config.ssid) + "</p>";
            page += "<p>Current URL: " + String(config.param1) + "</p>";
            if (strlen(config.param2) > 0) {
                page += "<p>Extra Parameter: " + String(config.param2) + "</p>";
            }
            page += "</div>";
        }

        // Form section
        page += R"(
                    <form method="post" action="/save">
                        <div class="field">
                            <label>WiFi SSID:</label>
                            <input type="text" name="ssid" required maxlength="32" value=")" + String(config.ssid) + R"(">
                        </div>
                        <div class="field">
                            <label>WiFi Password:</label>
                            <input type="password" name="password" maxlength="64" value=")" + String(config.password) + R"(">
                        </div>
                        <div class="field">
                            <label>Image URL:</label>
                            <input type="text" name="param1" required maxlength="128" value=")" + String(config.param1) + R"(">
                        </div>
                        <div class="field">
                            <label>Extra Parameter:</label>
                            <input type="text" name="param2" maxlength="32" value=")" + String(config.param2) + R"(">
                        </div>
                        <button type="submit">Save Configuration</button>
                    </form>
                </div>
            </body>
            </html>
        )";

        return page;
    }

public:
    SimpleWiFiManager() : server(80) {}

    bool begin()
    {
        Serial.println("Initializing WiFi Manager");

        // Read configuration
        if (!loadConfig())
        {
            Serial.println("No valid configuration found");
            startConfigMode();
            return false;
        }

        Serial.println("Valid configuration found");
        return connectToWiFi();
    }

    bool handleConfig()
    {
        if (!serverStarted)
        {
            Serial.println("Warning: Web server not started!");
            startConfigMode();
            return false;
        }

        dnsServer.processNextRequest();
        server.handleClient();

        // Output status every 10 seconds
        static unsigned long lastDebugTime = 0;
        unsigned long currentTime = millis();
        if (currentTime - lastDebugTime > 10000)
        {
            Serial.printf("AP Status: %s\n", WiFi.softAPIP().toString().c_str());
            Serial.printf("Connected stations: %d\n", WiFi.softAPgetStationNum());
            lastDebugTime = currentTime;
        }

        // Check configuration completion status
        if (configComplete)
        {
            Serial.println("Configuration completed, stopping server");
            dnsServer.stop();
            server.stop();
            serverStarted = false;
            return true;
        }

        // Check timeout
        if (currentTime - configStartTime > CONFIG_TIMEOUT)
        {
            Serial.println("Configuration timeout reached");
            dnsServer.stop();
            server.stop();
            serverStarted = false;
            ESP.restart();
        }

        return false;
    }

    bool isConfigured() { return config.configured; }
    String getParam1() { return String(config.param1); }
    String getParam2() { return String(config.param2); }

private:
    bool loadConfig()
    {
        if (!LittleFS.exists(CONFIG_FILE))
        {
            Serial.println("No configuration file found");
            return false;
        }

        File file = LittleFS.open(CONFIG_FILE, "r");
        if (!file)
        {
            Serial.println("Failed to open config file");
            return false;
        }

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, file);
        file.close();

        if (error)
        {
            Serial.println("Failed to parse config file");
            return false;
        }

        // Clear current configuration
        memset(&config, 0, sizeof(config));

        // Read configuration
        strlcpy(config.ssid, doc["ssid"] | "", sizeof(config.ssid));
        strlcpy(config.password, doc["password"] | "", sizeof(config.password));
        strlcpy(config.param1, doc["url"] | "", sizeof(config.param1));
        strlcpy(config.param2, doc["extra"] | "", sizeof(config.param2));
        config.configured = true;

        Serial.println("Configuration loaded successfully");
        Serial.printf("SSID: %s\n", config.ssid);
        Serial.printf("URL: %s\n", config.param1);

        return strlen(config.ssid) > 0 && strlen(config.param1) > 0;
    }

    bool saveConfig()
    {
        StaticJsonDocument<512> doc;

        doc["ssid"] = config.ssid;
        doc["password"] = config.password;
        doc["url"] = config.param1;
        doc["extra"] = config.param2;

        File file = LittleFS.open(CONFIG_FILE, "w");
        if (!file)
        {
            Serial.println("Failed to create config file");
            return false;
        }

        if (serializeJson(doc, file) == 0)
        {
            Serial.println("Failed to write config file");
            file.close();
            return false;
        }

        file.close();
        config.configured = true;

        Serial.println("Configuration saved successfully");
        return true;
    }

    bool connectToWiFi()
    {
        if (!config.configured || strlen(config.ssid) == 0)
        {
            Serial.println("No valid configuration for WiFi connection");
            return false;
        }

        WiFi.mode(WIFI_STA);
        WiFi.disconnect(true);
        delay(100);

        Serial.printf("Attempting to connect to SSID: %s\n", config.ssid);
        WiFi.begin(config.ssid, config.password);

        int timeout = 20;
        while (WiFi.status() != WL_CONNECTED && timeout > 0)
        {
            delay(1000);
            Serial.printf("WiFi status: %d\n", WiFi.status());
            Serial.print(".");
            timeout--;
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            Serial.println("\nWiFi Connected successfully");
            Serial.printf("IP Address: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }

        Serial.printf("\nFailed to connect to WiFi. Status: %d\n", WiFi.status());
        return false;
    }

    void startConfigMode()
    {
        Serial.println("Starting configuration mode...");

        WiFi.disconnect(true, true);
        delay(1000);
        WiFi.mode(WIFI_OFF);
        delay(1000);
        WiFi.mode(WIFI_AP);
        delay(1000);

        // Generate unique AP name
        uint8_t mac[6];
        WiFi.macAddress(mac);
        String ap_ssid = String("ESP32_") + String(mac[4], HEX) + String(mac[5], HEX);
        const char *ap_password = "12345678";

        Serial.println("===== WiFi Configuration Mode =====");
        Serial.printf("Setting up AP: %s\n", ap_ssid.c_str());
        Serial.printf("Password: %s\n", ap_password);

        WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));

        if (!WiFi.softAP(ap_ssid.c_str(), ap_password))
        {
            Serial.println("Failed to start AP mode");
            ESP.restart();
            return;
        }

        // Start DNS server
        dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

        IPAddress myIP = WiFi.softAPIP();
        Serial.printf("AP IP address: %s\n", myIP.toString().c_str());

        setupWebServer();
        server.begin();
        serverStarted = true;
        configStartTime = millis();

        Serial.println("HTTP server started");
        Serial.println("DNS server started");
        Serial.println("Waiting for configuration...");
        Serial.println("================================");
    }

    void setupWebServer()
    {
        // Handle all unknown requests - used for captive portal
        server.onNotFound([this]() {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
            server.send(302, "text/plain", "");
        });

        server.on("/", HTTP_GET, [this]()
                  {
            Serial.println("Web server: Root page requested");
            server.send(200, "text/html", getConfigPage()); });

        // Used for Apple device captive portal detection
        server.on("/generate_204", HTTP_GET, [this]() {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
            server.send(302, "text/plain", "");
        });

        server.on("/hotspot-detect.html", HTTP_GET, [this]() {
            server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
            server.send(302, "text/plain", "");
        });

        server.on("/save", HTTP_POST, [this]()
                  {
            Serial.println("Received configuration submission");
            
            String ssid = server.arg("ssid");
            String password = server.arg("password");
            String param1 = server.arg("param1");
            String param2 = server.arg("param2");

            Serial.printf("Received SSID: %s\n", ssid.c_str());
            Serial.printf("Received URL: %s\n", param1.c_str());

            if (ssid.length() > 0 && ssid.length() <= 32 && param1.length() > 0) {
                memset(&config, 0, sizeof(config));
                
                strlcpy(config.ssid, ssid.c_str(), sizeof(config.ssid));
                strlcpy(config.password, password.c_str(), sizeof(config.password));
                strlcpy(config.param1, param1.c_str(), sizeof(config.param1));
                strlcpy(config.param2, param2.c_str(), sizeof(config.param2));
                
                if (saveConfig()) {
                    server.send(200, "text/html", 
                        "<html><body><h1>Configuration saved successfully!</h1>"
                        "<p>Device will restart in a few seconds...</p></body></html>");
                    Serial.println("Configuration saved successfully");
                    configComplete = true;
                    delay(2000);
                    ESP.restart();
                } else {
                    server.send(500, "text/plain", "Failed to save configuration");
                    Serial.println("Failed to save configuration");
                }
            } else {
                server.send(400, "text/plain", "Invalid SSID or URL");
                Serial.println("Invalid SSID or URL received");
            } });
    }
};