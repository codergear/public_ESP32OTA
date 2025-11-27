// ESP32 Dev Module (WROOM

void setup() {
  pinMode(2, OUTPUT); // LED integrado en GPIO2
}

void loop() {
  digitalWrite(2, HIGH);
  delay(500);
  digitalWrite(2, LOW);
  delay(500);
}