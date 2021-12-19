#include <math.h>

// Used in simulation only.
int DEBUG = 1;
int DRY_RUN = 0;

const long kMsInSecond = 1000L;
const long kSecondsPerHour = 60L * 60L;
const long kSecondsPerDay = kSecondsPerHour * 24L;
const long kMinWaterInternal = kSecondsPerDay * 2L;

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

// Theoretically, this depends on the plant and sensitivity of the sensor.
int kMoistureThresholds[kPlantCount] = {0};

enum PumpStates {
    ON,
    OFF
};

class PumpStatus {
public:
    long duration_;
    PumpStates state_;

    bool IsOn() {
      return state_ == ON;
    }
    PumpStatus NextSecond() {
      return PumpStatus(state_, duration_ + 1);
    }
    PumpStatus(PumpStates state, int duration) :
      state_(state), duration_(duration) {}

    PumpStatus(PumpStates state) : PumpStatus(state, 0L) {}
};

int gLatestReadings[kPlantCount] = {0};
PumpStatus gPumpStates[kPlantCount] = { PumpStatus(OFF) };

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
    gPumpStates[i] = PumpStatus(OFF);
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

void Update(int plant_index, int moisture_level);

void PrintPumpStatus(PumpStatus status) {
  char text[50];
  sprintf(text, "Pump state: %d - Duration: %d seconds", status.state_, status.duration_);
  Serial.println(text);
}

// Repeated callback of Arduino.
void loop() {
  for (int i = 0; i < kPlantCount; i++) {
    int moisture_level = analogRead(kMoistureSensor[i]);
    Update(i, moisture_level);
    PrintPumpStatus(gPumpStates[i]);
  }
  PrintArray("Moisture readings: ", gLatestReadings, kPlantCount);
  delay(kMsInSecond);
}

// Supposed to be called once a second.
PumpStatus NextStatus(PumpStatus current_status, bool dry_enough) {
  const int duration = current_status.duration_;
  switch (current_status.state_) {
    case OFF:
      if (duration >= kMinWaterInternal) {
        Serial.println("Time-based trigger");
        return PumpStatus(ON);
      }
      if (dry_enough && duration >= kSecondsPerDay) {
        Serial.println("Moisture-based trigger");
        return PumpStatus(ON);
      }
      Serial.println("Remain OFF");
      return current_status.NextSecond();
    case ON:
      if (duration >= kMaxPumpingSeconds) {
        Serial.println("Saving water");
        return PumpStatus(OFF);
      }
      Serial.println("Remain ON");
      return current_status.NextSecond();
  }
}

// Supposed to be called once a second.
void Update(int plant_index, int moisture_level) {
  gLatestReadings[plant_index] = moisture_level;

  RollingArray *reading_history = gMoistureReadings[plant_index];
  reading_history->Add(moisture_level);
  reading_history->UpdateStats();
  if (DEBUG) {
    reading_history->Report();
  }
  // Only use the pump when the sensor's reading has stabilized.
  const bool dry_enough =
    (reading_history->Std() < 10.0) &&
    (reading_history->Avg() >= kMoistureThresholds[plant_index]);

  PumpStatus* status = &gPumpStates[plant_index];
  *status = NextStatus(*status, dry_enough);

  const int pump_command = status->IsOn() ? kPumpOn : kPumpOff;
  const int light_command = status->IsOn() ? kLightOn : kLightOff;
  if (!DRY_RUN) {
    digitalWrite(BUILT_IN_LED, light_command);
    digitalWrite(kPumpRelay[plant_index], pump_command);
  }
}
