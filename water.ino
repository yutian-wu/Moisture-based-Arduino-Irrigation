#include <math.h>

// Used in simulation only.
int DEBUG = 1;
int DRY_RUN = 0;

const int kMsInSecond = 1000;
const int kMsInHour = kMsInSecond * 60 * 60;

const int kPlantCount = 1;

// Values are defined by Arduino.
const int BUILT_IN_LED = 13;
const int kPumpOn = LOW;
const int kPumpOff = HIGH;
const int kLightOn = HIGH;
const int kLightOff = LOW;

// Currently using a 4-channel relay module.
// const int kPumpRelay[kPlantCount] = {1, 2, 3, 4};
const int kPumpRelay[kPlantCount] = {4};

// readings from A0, A1 are unreliable.
// Constants are defined by Arduino.
//const int kMoistureSensor[kPlantCount] = {A2, A3, A4, A5};
const int kMoistureSensor[kPlantCount] = {A5};

// Wetter -> lower reading.
const int kMoistureThreshold = 450;
const int kInitialMoisture = 200;
const int kMaxPumpingSeconds = 5;

// TODO: implement water-saver based on irrigation interval.
const int kMinWaterInternal = kMsInHour * 24;

// Theoretically, this depends on the plant and sensitivity of the sensor.
int kMoistureThresholds[kPlantCount] = {0};

int gLatestReadings[kPlantCount] = {0};
int gPumpStates[kPlantCount] = {0};
int gSecondsInState[kPlantCount] = {0};
int gLastPumpTimeMs = 0;

void PrintDouble(double val, unsigned int precision) {
  Serial.print(int(val));  //prints the int part
  Serial.print("."); // print the decimal point
  unsigned int frac;
  if (val >= 0) {
    frac = (val - int(val)) * precision;
  } else {
    frac = (int(val) - val) * precision;
  }
  Serial.println(frac, DEC);
}

void PrintArray(const char *title, int *arr, int len) {
  if (!DEBUG) {
    return;
  }
  Serial.print(title);
  for (int i = 0; i < len; i++) {
    Serial.print(arr[i]);
    Serial.print(" ");
  }
  Serial.println();
}

#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

class RollingArray {
    int size_;
    int *ptr_;

    double avg_;
    double std_;
public:
    const int kMin;
    const int kMax;
    const int kCapacity;

    RollingArray(int capacity, int min, int max) : kCapacity(capacity), kMin(min), kMax(max) {
      this->ptr_ = new int[kCapacity];
      for (int i = 0; i < capacity; i++) {
        this->ptr_[i] = 0;
      }
      this->size_ = 0;
    }

    ~RollingArray() {
      if (ptr_ != 0) {
        delete ptr_;
      }
      ptr_ = 0;
    }

    // return 0 if success or 1 otherwise
    int Add(int val) {
      if (val < kMin || val > kMax) {
        return 1;
      }
      int index = size_ % this->kCapacity;
      this->ptr_[index] = val;
      size_ = size_ + 1;
      return 0;
    }

    void UpdateStats() {
      int sum = 0;
      int total = MIN(kCapacity, size_);
      for (int i = 0; i < total; i++) {
        int val = this->ptr_[i];
        sum += val;
      }
      this->avg_ = sum / total;

      // We have a limited number of elements and each cannot exceed 1024, so this won't cause overflow.
      int sqr_sum = 0;
      for (int i = 0; i < total; i++) {
        int val = this->ptr_[i];
        sqr_sum += sq(val - this->avg_);
      }
      // PrintArray("readings: ", this->ptr_, this->kCapacity);
      this->std_ = sqrt(sqr_sum / (double) total);
    }

    void Report() {
      Serial.print("Average: ");
      Serial.print(this->avg_);
      Serial.print(" Std: ");
      PrintDouble(this->std_, 3);
      Serial.println();
    }

    double Avg() {
      return this->avg_;
    }

    double Std() {
      return this->std_;
    }
};

RollingArray **gMoistureReadings;

// One-off initial callback of Arduino.
void setup() {
  Serial.begin(9600);
  gMoistureReadings = new RollingArray *[kPlantCount];
  for (int i = 0; i < kPlantCount; i++) {
    kMoistureThresholds[i] = kMoistureThreshold;
    gLatestReadings[i] = kInitialMoisture;
    // Turn off the pump initially.
    gPumpStates[i] = 0;
    gSecondsInState[i] = 0;
    // Used to track historical moisture readings.
    gMoistureReadings[i] = new RollingArray(60, 0, 1024);
  }
  for (int i = 0; i < kPlantCount; i++) {
    pinMode(kPumpRelay[i], OUTPUT);
    digitalWrite(kPumpRelay[i], kPumpOff);
    pinMode(kMoistureSensor[i], INPUT);
  }
  pinMode(BUILT_IN_LED, OUTPUT);
  delay(kMsInSecond);
}

void UpdatePumpState(int plant_index, int moisture_level);

// Repeated callback of Arduino.
void loop() {
  for (int i = 0; i < kPlantCount; i++) {
    int moisture_level = analogRead(kMoistureSensor[i]);
    UpdatePumpState(i, moisture_level);
  }
  PrintArray("Moisture readings: ", gLatestReadings, kPlantCount);
  PrintArray("Pump states        ", gPumpStates, kPlantCount);
  PrintArray("Seconds in state:  ", gSecondsInState, kPlantCount);

  delay(kMsInSecond);
}

bool ShouldPumpWater(int plantIndex, int prev_state, int new_state) {
  // OFF/ON -> OFF
  if (new_state != 1) {
    digitalWrite(BUILT_IN_LED, kLightOff);
    return false;
  }
  // OFF -> ON
  if (prev_state != 1) {
    return true;
  }
  // ON -> ON
  if (gSecondsInState[plantIndex] <= kMaxPumpingSeconds) {
    return true;
  }
  // Turn on warning light
  digitalWrite(BUILT_IN_LED, kLightOn);
  return false;
}

// Supposed to be called once a second.
void UpdatePumpState(int plant_index, int moisture_level) {
  gLatestReadings[plant_index] = moisture_level;

  RollingArray *reading_history = gMoistureReadings[plant_index];
  reading_history->Add(moisture_level);
  reading_history->UpdateStats();
  if (DEBUG) {
    reading_history->Report();
  }
  // Only use the pump when the sensor's reading has stabilized.
  const int use_pump =
    (reading_history->Std() < 10.0) &&
    (reading_history->Avg() >= kMoistureThresholds[plant_index]);

  int prev_state = gPumpStates[plant_index];
  if (prev_state == use_pump) {
    gSecondsInState[plant_index] = gSecondsInState[plant_index] + 1;
  } else {
    gPumpStates[plant_index] = use_pump;
    gSecondsInState[plant_index] = 0;
  }
  if (ShouldPumpWater(plant_index, prev_state, use_pump) && !DRY_RUN) {
    const int pump_command = gPumpStates[plant_index] ? kPumpOn : kPumpOff;
    digitalWrite(kPumpRelay[plant_index], pump_command);
  }
}
