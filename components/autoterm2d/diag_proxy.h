#pragma once
// ============================================================================
//  diag_proxy.h  –  TCP-Diagnosedurchleitung für ha_autoterm
//  Fasst die Heizungs-UART in einen WiFi-TCP-Socket, sodass die offizielle
//  Autoterm Test-Software über einen virtuellen COM-Port zugreifen kann.
//
//  Protokoll:
//    – Port 8888 (konfigurierbar per set_diag_port())
//    – Roher Byte-Stream, kein Framing – identisch zum physischen COM-Port
//    – Baud-Rate 9600 8N1 in der Diagnosesoftware einstellen
//
//  Verhalten je nach Betriebsmodus:
//    Bridge-Modus (physisches Bedienteil vorhanden):
//      Bedienteil treibt weiterhin den Poll-Zyklus. Der TCP-Client empfängt
//      alle Heizungsantworten und kann eigene Befehle einspeisen.
//
//    Virtuell-Panel-Modus (kein physisches Bedienteil):
//      Sobald ein TCP-Client verbunden ist, pausiert der interne Poll-Zyklus.
//      Nach Trennung des Clients nimmt der ESP32 den Poll-Betrieb wieder auf.
//
//  Implementierung:
//    Verwendet lwip-BSD-Sockets statt WiFiServer/WiFiClient. Damit läuft der
//    Code sowohl unter dem Arduino- als auch unter dem ESP-IDF-Framework.
//    ESPHome 2026.7+ baut ESP32-Ziele mit ESP-IDF, wo die Arduino-Header
//    WiFiServer.h / WiFiClient.h nicht existieren.
//
//  Sicherheit:
//    Der Server akzeptiert genau einen Client gleichzeitig.
//    Kein Auth – Zugriffsschutz über Netzwerksegmentierung (VLAN/Firewall).
// ============================================================================

#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <functional>
#include <string>
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

namespace esphome {
namespace autoterm2d {

static const char *const DIAG_TAG = "diag_proxy";

// ---------------------------------------------------------------------------
//  Mixin-Klasse  –  in Autoterm2DClimate als zusätzliche Basis einbinden
// ---------------------------------------------------------------------------
class DiagProxyMixin {
 public:
  // ── Konfiguration (optional, Standard: Port 8888) ────────────────────────
  void set_diag_port(uint16_t port) { diag_port_ = port; }

  // ── Status-Abfrage ────────────────────────────────────────────────────────
  /// true solange ein TCP-Diagnoseclient verbunden ist
  bool is_diagnostic_active() const { return client_fd_ >= 0; }

  /// IP-Adresse des verbundenen Clients (oder "" wenn keiner)
  std::string diag_client_ip() const {
    return (client_fd_ >= 0) ? client_ip_ : std::string();
  }

 protected:
  // ── Lifecycle ─────────────────────────────────────────────────────────────

  /// In loop() aufrufen – startet den TCP-Server sobald das Netzwerk steht.
  ///
  /// WICHTIG: Nicht in setup() starten. Komponenten mit setup_priority::DATA
  /// (600) laufen VOR der WiFi-Komponente (250). Unter ESP-IDF ist der
  /// lwIP-Stack zu diesem Zeitpunkt noch nicht initialisiert – ein
  /// ::socket()-Aufruf schlägt dort fehl bzw. bricht den Boot ab.
  /// Unter Arduino fiel das nicht auf, weil das Framework lwIP früh startet.
  void diag_ensure_started_() {
    if (diag_started_ || !network::is_connected()) return;
    diag_started_ = true;
    diag_setup_();
  }

  /// Legt den Server-Socket an (wird von diag_ensure_started_() aufgerufen)
  void diag_setup_() {
    server_fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd_ < 0) {
      ESP_LOGE(DIAG_TAG, "socket() fehlgeschlagen (errno %d) – Proxy inaktiv", errno);
      return;
    }

    int yes = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(diag_port_);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
      ESP_LOGE(DIAG_TAG, "bind() auf Port %u fehlgeschlagen (errno %d)", diag_port_, errno);
      close_server_();
      return;
    }
    if (::listen(server_fd_, 1) != 0) {
      ESP_LOGE(DIAG_TAG, "listen() fehlgeschlagen (errno %d)", errno);
      close_server_();
      return;
    }
    set_nonblocking_(server_fd_);

    ESP_LOGI(DIAG_TAG,
             "TCP-Diagnoseserver läuft auf Port %u  (Baud 9600 8N1 in der "
             "Diagnosesoftware einstellen)",
             diag_port_);
  }

  /// Am Anfang von loop() aufrufen – verwaltet Client-Verbindungen
  /// Gibt true zurück wenn ein Client verbunden ist
  bool diag_loop_tick_() {
    if (server_fd_ < 0) return false;

    // ── Neue Verbindung annehmen ──────────────────────────────────────────
    if (client_fd_ < 0) {
      struct sockaddr_in caddr;
      socklen_t clen = sizeof(caddr);
      memset(&caddr, 0, sizeof(caddr));
      int fd = ::accept(server_fd_, reinterpret_cast<struct sockaddr *>(&caddr), &clen);
      if (fd >= 0) {
        set_nonblocking_(fd);
        int flag = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        client_fd_ = fd;

        char ipbuf[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &caddr.sin_addr, ipbuf, sizeof(ipbuf)) != nullptr)
          client_ip_ = ipbuf;
        else
          client_ip_ = "?";

        ESP_LOGI(DIAG_TAG,
                 "Diagnose-Client verbunden: %s  –  Poll-Zyklus pausiert",
                 client_ip_.c_str());
      }
    }

    return client_fd_ >= 0;
  }

  /// Nach dem Lesen eines UART-Bytes aus der Heizung aufrufen:
  ///   read_byte(&b);
  ///   diag_forward_rx_(b);     ← dieses hier
  /// Leitet das Byte zusätzlich an den TCP-Client weiter.
  void diag_forward_rx_(uint8_t byte) {
    if (client_fd_ < 0) return;
    int n = ::send(client_fd_, &byte, 1, 0);
    if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN)
      disconnect_client_("Schreibfehler");
  }

  /// Liest Bytes vom TCP-Client und übergibt sie an uart_write_fn.
  /// uart_write_fn(b) soll das Byte auf UART2 TX senden.
  /// Max. 64 Bytes pro Loop-Tick (kein Watchdog-Risiko).
  void diag_drain_tx_(const std::function<void(uint8_t)> &uart_write_fn) {
    if (client_fd_ < 0) return;
    uint8_t buf[64];
    int n = ::recv(client_fd_, buf, sizeof(buf), 0);
    if (n > 0) {
      for (int i = 0; i < n; i++) uart_write_fn(buf[i]);
    } else if (n == 0) {
      disconnect_client_("Client hat Verbindung geschlossen");
    } else if (errno != EWOULDBLOCK && errno != EAGAIN) {
      disconnect_client_("Lesefehler");
    }
  }

 private:
  uint16_t    diag_port_{8888};
  bool        diag_started_{false};
  int         server_fd_{-1};
  int         client_fd_{-1};
  std::string client_ip_;

  static void set_nonblocking_(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  void close_server_() {
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
  }

  void disconnect_client_(const char *reason) {
    if (client_fd_ >= 0) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
    client_ip_.clear();
    ESP_LOGI(DIAG_TAG,
             "Diagnose-Client getrennt (%s)  –  normaler Betrieb wird fortgesetzt",
             reason);
  }
};

}  // namespace autoterm2d
}  // namespace esphome
