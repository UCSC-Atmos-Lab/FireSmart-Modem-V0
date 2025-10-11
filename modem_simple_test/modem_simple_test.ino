#define Serial_RX_PIN       20
#define Serial_TX_PIN       21

void setup() {
  Serial.begin(9600, SERIAL_8N1, Serial_RX_PIN, Serial_TX_PIN);
}

void loop() {
  Serial.println("Hello World!");
  delay(1000);

}
