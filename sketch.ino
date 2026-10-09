const int SOIL_PIN = 26;   // GP26 (ADC0)
const int LIGHT_PIN = 27;  // GP27 (ADC1)

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);  // 0-4095
}

void loop() {
  float soil = analogRead(SOIL_PIN) / 4095.0;
  float light = analogRead(LIGHT_PIN) / 4095.0;

  Serial.print("soil: ");
  Serial.print(soil);
  Serial.print("  light: ");
  Serial.println(light);

  delay(1000);
}
//placeholder code for it. 
//the parts that are a placeholder is the pi pico for a pi 5, potentiometer is the placeholder for the soil motion sensor
//photoresistor sensor is the placeholder for the ldr light module, pi pico contains the pins for the mpc 3008 adc