// Configuración del reloj de hardware. Tiene que ir antes de los includes
// Tomado de https://github.com/khoih-prog/ESP8266TimerInterrupt
#define USING_TIM_DIV1                true            // for shortest and most accurate timer
#define USING_TIM_DIV16               false            // for medium time and medium accurate timer
#define USING_TIM_DIV256              false             // for longest timer but least accurate. Default
#define HW_TIMER_INTERVAL_MS          50L


// Incluír todas las cabeceras
#include <Adafruit_GFX.h>     
#include <Adafruit_ILI9341.h>  
#include <Arduino.h>
#include <ctype.h>
#include "ESP8266TimerInterrupt.h"
#include "ESP8266_ISR_Timer.h"
#include <ESP8266WiFi.h>
#include <math.h>
#include <string.h>
#include <SPI.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>


// CONSTANTES  ------------------------------------------------------------------------------------
// Constantes. F() es para que el compilador guarde los strings en memoria FLASH, y no use RAM.
#define WIFI_SSID "XXXXXXXXXXXXXXXX"
#define WIFI_PASSWD "XXXXXXXXXXXXXXXX"
#define API_HOST "api.ipgeolocation.io"
#define API_PORT 443
#define API_PATH "/timezone?apiKey=XXXXXXXXXXXXXXXXXXXXXXXXXXXXXX&tz="
#define LONG_LINEA 12 // Longitud máxima de la línea de texto en pantalla
#define TAM_LETRA 3
#define ANCHO_LETRA (6 * TAM_LETRA)
#define ALTO_LETRA (8 * TAM_LETRA)

// Pines usados por la pantalla
#define TFT_CS    -1     // La línea Chip Select no se usa
#define TFT_RST   D3     // Reset está conectado a D3
#define TFT_DC    D4     // La línea Dato/Comando está conectada a D4
#define TFT_CLK   D5     // El reloj de SPI está conecctado a D5
#define TFT_MISO  D6     // La línea de datos Pantalla -> ESP está conectada a D6 
#define TFT_MOSI  D7

// Definición de colores
#define NEGRO    0x0000
#define AZUL     0x001F
#define ROJO     0xF800
#define VERDE    0x07E0
#define CIAN     0x07FF
#define MAGENTA  0xF81F
#define AMARILLO 0xFFE0 
#define BLANCO   0xFFFF

// DEFINICIONES -----------------------------------------------------------------------------------
// Estructura que guarda la información de cada ciudad
typedef struct {
  const char *nombre;
  const char *identificador;
  long long segundos;
} lugar_t;


// VARIABLES --------------------------------------------------------------------------------------
// Lista de ciudades
bool dibujar_horas;

lugar_t ciudades[10] = {
  {.nombre = "SFO", .identificador = "US/Pacific", .segundos = 0},
  {.nombre = "AUS", .identificador = "US/Central", .segundos = 0},
  {.nombre = " NY", .identificador = "US/Eastern", .segundos = 0},
  {.nombre = "BUE", .identificador = "America/Argentina/Buenos_Aires", .segundos = 0},
  {.nombre = "DUB", .identificador = "Europe/Dublin", .segundos = 0},
  {.nombre = "LON", .identificador = "Europe/London", .segundos = 0},
  {.nombre = "ZRH", .identificador = "Europe/Zurich", .segundos = 0},
  {.nombre = "BLR", .identificador = "Asia/Kolkata", .segundos = 0},
  {.nombre = "TOK", .identificador = "Asia/Tokyo", .segundos = 0},
  {.nombre = "SYD", .identificador = "Australia/Sydney", .segundos = 0}
};
#define NUM_CIUDADES (sizeof(ciudades) / sizeof(lugar_t))

// Variables que representan el reloj de tiempo real del ESP8266
ESP8266Timer ITimer;
ESP8266_ISR_Timer ISR_Timer;

// Pantalla
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// CÓDIGO -----------------------------------------------------------------------------------------

// El módulo de reloj llama a esta función cada vez que el reloj de hardware completa un ciclo.
// Lo único que hace es pasar la llamada el módulo que lleva la cuenta de los milisegundos.
void IRAM_ATTR TimerHandler()
{
  ISR_Timer.run();
}


// Actualiza los segundos 
void IRAM_ATTR segundo() {
  int i;

  // Sumar 1 segundo a todos los contadores
  for (i = 0; i < NUM_CIUDADES; i++) {
    ciudades[i].segundos += 1;
  }

  /* Como el programa llama a las API de ipgeolocation de a 1 ciudad a la vez,
     es posible que algunas ciudades estén a +/- 1 segundo de otras 
     (por la diferencia entre cuando cambia el segundo localmente y remotamente)
     así que cuando el segundero de la primer ciudad se hace == 30, todas las ciudades
     se sincronizan a :30
  */
  if (ciudades[0].segundos % 60 == 30) {
    for (i = 0; i < NUM_CIUDADES; i++) {
      ciudades[i].segundos = ciudades[i].segundos - (ciudades[i].segundos % 60) + 30;
    }
  }

  /* Muchos países cambian los horarios en verano e invierno,
     adelantando o atrasando 1 hora.
     Para compensar, a la 1am de cada día reiniciar todo para cargar las horas nuevas.
     Así no hay que programar de antemano ningún cronograma de cambios de horas, 
     ni mantener nada actualizado. La API de ipgeolocation se encarga.
     Esto también sirve para que el reloj interno no se desfase mucho de la hora real,
     porque se sincroniza cada 24 horas.
  */
  if ( ciudades[4].segundos % 60 == 01
       && (int)((ciudades[4].segundos / 60) % 60) == 01
       && (int)((ciudades[4].segundos / (60 * 60)) % 24) == 01 ) {
         // A las 01:00:01, resetear el microcontrolador.
         
         ESP.restart();
  }

  // Señalar que hay que redibujar la pantalla cuando los segundos son 0
  if (ciudades[0].segundos % 60 == 0) {
    dibujar_horas = true;
  }
}


long long tomar_dd(char * texto) {
  return (texto[0] - '0') * 10 + 
          texto[1] - '0';
}


long long tomar_hhmmss(char *texto) {
  return tomar_dd(texto) * 60 * 60 +  // horas
         tomar_dd(texto + 3) * 60  +  // minutos
         tomar_dd(texto + 6);         // segundos
}


/*
 * Llamar al servicio web que carga la hora actual en cada lugar
 */ 
void inicializar_segundos() {
  WiFiClientSecure httpsClient;
  char *comienzo_hora;

  memoria_libre();

  Serial.println(F("Actualizando horas de ciudades:"));
  httpsClient.setTimeout(5000);
  httpsClient.setInsecure();
 
  // Actualizar los segundos de todas las ciudades
  for (int i = 0; i < NUM_CIUDADES; i++) {
    Serial.println(String(F("----- ")) + ciudades[i].nombre + F(" -----"));

    Serial.print(F("Conectando a "));
    Serial.println(API_HOST);

    // Crear la conexión HTTPS al servidor
    while(!httpsClient.connect(API_HOST, API_PORT)) {
        delay(100);
    }

    // Crear el pedido HTTP
    String request = String("GET ") + API_PATH + ciudades[i].identificador + F(" HTTP/1.1\r\n") +
        F("Host: ") + API_HOST + F("\r\n" 
        "Content-Type: APPLICATION/JSON\r\n" 
        "Connection: close\r\n\r\n" 
        "\r\n\r\n");

    // Enviar el pedido
    httpsClient.print(request);

    // Leer la cabecera de la respuesta hasta llegar a una línea vacía.
    while(httpsClient.connected()) {
        String linea = httpsClient.readStringUntil('\n');
        if(linea == F("\r")) {
            break;
        }
    }

    // Leer el cuerpo de la respuesta
    while(httpsClient.available()) {
        String linea = httpsClient.readStringUntil('\n').c_str();
        Serial.println(linea);
    
        if (linea[0] == '{') {
          // La línea es la estructura JSON de respuesta. Extraer el timestamp.

          // 1) encontrar dónde empieza el valor date_time_unix
          comienzo_hora = strstr(linea.c_str(), "time_24") + 10; 
            ciudades[i].segundos = tomar_hhmmss(comienzo_hora);
        }
    }
  }
}


// Inicizar el reloj, registrar la función que se llama todos los minutos
void comenzar_a_contar() {
  if (ITimer.attachInterruptInterval(HW_TIMER_INTERVAL_MS * 1000, TimerHandler))
  {
    Serial.print(F("Starting ITimer OK, millis() = ")); Serial.println(millis());
  }
    Serial.println(F("Can't set ITimer. Select another freq. or timer"));

  // Registrar la función que se llama cada segundo (1.000 milisegundos)
  ISR_Timer.setInterval(1000L, segundo);
}


void setup() {
  delay(100);
  dibujar_horas = true;

  // Código de inicialización:
  // Inicializar puerto serie
  Serial.begin(115200);
  Serial.print(F("En setup()\n"));

  // Inicializar la pantalla
  tft.begin();
  tft.cp437(true);
  tft.setRotation(0);
  tft.fillScreen(NEGRO);
  tft.setTextColor(BLANCO, NEGRO);
  tft.setTextSize(TAM_LETRA);

  imprimir_linea(0, F("Iniciando...           "));

  // Inicializar WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWD);

  // Connectando a WiFi...
  Serial.print(F("Conectando a "));
  Serial.println(WIFI_SSID);
 
  // Loop continuously while WiFi is not connected
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.print(".");
  }
   
  // Conexión a WiFi completa
  Serial.println(F("Conectado! Dirección IP: "));
  Serial.println(WiFi.localIP());

  comenzar_a_contar();

  inicializar_segundos();
}


void imprimir_linea(const int linea, const void *string) {

  int xpos = 0;

  tft.setCursor(linea * ALTO_LETRA, 0);
  tft.println((char *)string);
}


void refrescar_pantalla() {
  int i;
  char texto[16];

  int segundos;
  int minutos; 
  int horas;

  tft.setCursor(0, 0);
  tft.setTextColor(BLANCO, NEGRO);
  tft.println(F("  MULTIRELOJ "));
  tft.println(F(""));

  for (i = 0; i < NUM_CIUDADES; i++) {
      segundos = ciudades[i].segundos;
      minutos = (segundos / 60) % 60;
      horas = (segundos / (60*60)) % 24;

      if (horas >=9 && horas < 17) {
        tft.setTextColor(VERDE, NEGRO);
      } else {
        tft.setTextColor(ROJO, NEGRO);      
      }

      snprintf(texto, 16, "  %s: %02d:%02d", ciudades[i].nombre, horas, minutos);
      tft.println(texto);
  }

  printf ("\n");
}


void loop() {
  if (dibujar_horas) {
    refrescar_pantalla();
    dibujar_horas = false;
  }

  delay(100);
}
