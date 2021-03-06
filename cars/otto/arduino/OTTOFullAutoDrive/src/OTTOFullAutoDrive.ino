/*
    OTTO REmote Drive:
    Drive the oTTO Tractor with a Remote RC Controller
   by Rick Anderson (ricklon)

*/
#include <Arduino.h>
#include <SoftPWMServo.h>

#define DEBUG_SERIAL false
#define MAX_CMD_BUF  20

/*
    Automatic Set Up
*/
#define CMD_AUTO 0
#define CMD_STR 1
#define CMD_THR 2
#define CMD_TIME 3

#define THR_DIR 0
#define THR_THR 1

unsigned long last_serial_time;
unsigned long last_time;


//Setup Motor Controller
const int PIN_M1_DIR = 14;
const int PIN_M2_DIR = 5; 
const int PIN_M1_PWM = 4;
const int PIN_M2_PWM = 7;
const int PIN_KILL = 23;

//Setup Steering Control
const int PIN_STR = 10;

//shoot through delay
int PREV_DIR = LOW;
const int SHOOT_DELAY = 250;
//this will be toggled with a channel from RC. Allows user to set speed using RC
bool manual_speed=false; 
// possible states car can be in 
enum carStateEnumeration{
    STATE_MANUAL=0,
    STATE_AUTONOMOUS=1,
    STATE_TERM_AUTO=2
};
//list of commands to be sent or received serially
enum commandEnumeration{
	NOT_ACTUAL_COMMAND=0,
	RC_SIGNAL_WAS_LOST=1,
	RC_SIGNALED_STOP_AUTONOMOUS=2,
	STEERING_VALUE_OUT_OF_RANGE=3,
	THROTTLE_VALUE_OUT_OF_RANGE=4,
	RUN_AUTONOMOUSLY=5,
	STOP_AUTONOMOUS=6,
	STOPPED_AUTO_COMMAND_RECEIVED=7,
        NO_COMMAND_AVAILABLE=8,
        GOOD_PI_COMMAND_RECEIVED=9,
	TOO_MANY_VALUES_IN_COMMAND=10,
	GOOD_RC_SIGNALS_RECEIVED=11
};
struct commandDataStruct{
int command;
int16_t ax;
int16_t ay;
int16_t az;
int16_t gx;
int16_t gy;
int16_t gz;
unsigned long time;
int str;
int thr;
};

int gcarState;
int gTheOldRCcommand;
int gTheOldPiCommand;
//these values will be car specific
const unsigned long minimumSteeringValue = 1000;
const unsigned long maximumSteeringValue = 2000;
const unsigned long minimumThrottleValue = 1000;
const unsigned long maximumThrottleValue = 2000;
const unsigned long steeringThresholdToShutdownAuto = 1600;

unsigned long gCenteredSteeringValue;
unsigned long gCenteredThrottleValue;

/*
   Setup RC Controller
   What are the channels:
   0: thr: throttle
   1: str, steering
   2: kill: pos1: OK, pos:2 Emergency Stop
   3: auto: pos1: Manual, pos: AUTO

*/
#define RC_INPUT_STR 0
#define RC_INPUT_THR 1
#define RC_INPUT_KILL 2
#define RC_INPUT_MAN 3
#define RC_INPUT_COUNT 4

volatile uint16_t pulseHighTime[RC_INPUT_COUNT];
volatile uint16_t pulseLowTime[RC_INPUT_COUNT];

unsigned long steer_history[20]; //Array to store 1/5 of a second of steer values
int steer_next_ind; //index of next value to be written in steer_history
unsigned long thr_zero_val=1533;

unsigned long compAvg(unsigned long *data_array, int len){
    unsigned long result=0;
    for(int i=0; i<len; i++){
        result+=data_array[i];
    }
    return (result/len);
}

//This function pulls the data being populated by the input capture interrupts.
//it corrects for the timer restarting.
inline int pulseRead(int RCindex){return (pulseHighTime[RCindex]>0)?(int)(0.8*pulseHighTime[RCindex]):(int)(0.8*pulseHighTime[RCindex]+0xFFFF);}
//inline int pulseRead(int RCindex){return (int)(0.8*pulseHighTime[RCindex]);}

//interrupt service routine for first input capture module
void __USER_ISR InputCaptureSTR_ISR(void) {
  static uint16_t risingEdgeTime = 0;
  static uint16_t fallingEdgeTime = 0;
  
  clearIntFlag(_INPUT_CAPTURE_1_IRQ);
  if (IC1CONbits.ICBNE == 1)
  {
    if (digitalRead(RC_INPUT_STR) == HIGH)
    {
      risingEdgeTime = IC1BUF;
      pulseLowTime[0] = risingEdgeTime - fallingEdgeTime;
    }
    else
    {
      fallingEdgeTime = IC1BUF;
      pulseHighTime[0] = fallingEdgeTime - risingEdgeTime;
    }
  }
}

//interrupt service routine for second input capture module
void __USER_ISR InputCaptureTHR_ISR(void) {
  static uint16_t risingEdgeTime = 0;
  static uint16_t fallingEdgeTime = 0;
  
  clearIntFlag(_INPUT_CAPTURE_2_IRQ);
  if (IC2CONbits.ICBNE == 1)
  {
    if (digitalRead(RC_INPUT_THR) == HIGH)
    {
      risingEdgeTime = IC2BUF;
      pulseLowTime[1] = risingEdgeTime - fallingEdgeTime;
    }
    else
    {
      fallingEdgeTime = IC2BUF;
      pulseHighTime[1] = fallingEdgeTime - risingEdgeTime;
    }
  }
}

//interrupt service routine for third input capture module
void __USER_ISR InputCaptureKILL_ISR(void) {
  static uint16_t risingEdgeTime = 0;
  static uint16_t fallingEdgeTime = 0;
  
  clearIntFlag(_INPUT_CAPTURE_3_IRQ);
  if (IC3CONbits.ICBNE == 1)
  {
    if (digitalRead(RC_INPUT_KILL) == HIGH)
    {
      risingEdgeTime = IC3BUF;
      pulseLowTime[2] = risingEdgeTime - fallingEdgeTime;
    }
    else
    {
      fallingEdgeTime = IC3BUF;
      pulseHighTime[2] = fallingEdgeTime - risingEdgeTime;
    }
  }
}

//interrupt service routine for fourth input capture module
void __USER_ISR InputCaptureMAN_ISR(void) {
  static uint16_t risingEdgeTime = 0;
  static uint16_t fallingEdgeTime = 0;
  
  clearIntFlag(_INPUT_CAPTURE_4_IRQ);
  if (IC4CONbits.ICBNE == 1)
  {
    if (digitalRead(RC_INPUT_MAN) == HIGH)
    {
      risingEdgeTime = IC4BUF;
      pulseLowTime[3] = risingEdgeTime - fallingEdgeTime;
    }
    else
    {
      fallingEdgeTime = IC4BUF;
      pulseHighTime[3] = fallingEdgeTime - risingEdgeTime;
    }
  }
}

void handleRCSignals(commandDataStruct *theDataPtr){
  theDataPtr->time=millis();
  unsigned long STR_VAL=pulseRead(0);
  unsigned long THR_VAL=pulseRead(1);
  unsigned long KILL_VAL=pulseRead(2);
  unsigned long MAN_VAL=pulseRead(3);
  if (KILL_VAL > 1500 ) {
    digitalWrite(PIN_KILL, HIGH);
  }
  else {
    digitalWrite(PIN_KILL, LOW);
  }

  if (MAN_VAL > 1500 && MAN_VAL < 1900) {
    manual_speed=true; //have speed controlled manually
  }else{
    manual_speed=false;
  }
  if (STR_VAL==0){
    if(gTheOldRCcommand!=RC_SIGNAL_WAS_LOST){
      gTheOldRCcommand=RC_SIGNAL_WAS_LOST;
    }
    theDataPtr->command=RC_SIGNAL_WAS_LOST;
    return;
  }
  if(gcarState==STATE_AUTONOMOUS){
    if(STR_VAL>steeringThresholdToShutdownAuto){
      theDataPtr->command=RC_SIGNALED_STOP_AUTONOMOUS;
      return;
    }
  }

  if( STR_VAL > maximumSteeringValue )
      STR_VAL = maximumSteeringValue;

  else if( STR_VAL < minimumSteeringValue )
      STR_VAL = minimumSteeringValue;

  if( THR_VAL > maximumThrottleValue )
      THR_VAL = maximumThrottleValue;

  else if( THR_VAL < minimumThrottleValue )
      THR_VAL = minimumThrottleValue;
//----------------FILTERING/CLIPPING HERE-------------------
  long thr_dif=long(THR_VAL)-long(thr_zero_val);
  steer_history[steer_next_ind]=STR_VAL;
  steer_next_ind=(steer_next_ind+1)%20;
  unsigned long FILT_STR_VAL=compAvg(steer_history, 20);

  unsigned long CLIP_THR_VAL;
  
  if (manual_speed==true){
    CLIP_THR_VAL=THR_VAL;
  }else{
    if(thr_dif>50){
        CLIP_THR_VAL=1635;
    }else if (thr_dif<-50){
        CLIP_THR_VAL=1425;
    }else{
        CLIP_THR_VAL=thr_zero_val;
    }
  }
    //---------------------------------------------------------
  theDataPtr->ax=0;
  theDataPtr->ay=0;
  theDataPtr->az=0;

  // Gyroscope
  theDataPtr->gx=0;
  theDataPtr->gy=0;
  theDataPtr->gz=0;
    
  theDataPtr->thr = (int) CLIP_THR_VAL;
  theDataPtr->str = (int) FILT_STR_VAL;
  theDataPtr->time = millis();
  theDataPtr->command = GOOD_RC_SIGNALS_RECEIVED;
}

void getSerialCommandIfAvailable( commandDataStruct *theDataPtr ){
    // http://arduino.stackexchange.com/questions/1013/how-do-i-split-an-incoming-string
    int cmd_cnt = 0;
    char cmdBuf[MAX_CMD_BUF];
                
    //Serial.flush();
    if (Serial.available()) {        
        Serial.println("Found serial command");
        byte size = Serial.readBytes(cmdBuf, MAX_CMD_BUF);
        
        if (DEBUG_SERIAL) {
            Serial.write(cmdBuf, size);    //echo what the Pi sent right back to it
        }
            
        // tack on a null byte to the end of the line
        cmdBuf[size] = 0;
    
        // strtok splits a C string into substrings, based on a separator character
        char *command = strtok(cmdBuf, ",");    //  get the first substring

        // loop through the substrings, exiting when the null byte is reached
        //    at the end of each pass strtok gets the next substring
        
        while (command != 0) {        
            switch (cmd_cnt) {
            case CMD_AUTO:
                theDataPtr->command = atoi(command);
                Serial.print("debug command:    ");
                Serial.println(theDataPtr->command);
                break;
            case CMD_STR:
                theDataPtr->str = atoi(command);    
                if( theDataPtr->str > 2000 || theDataPtr->str < 1000 ){
                    theDataPtr->command = STEERING_VALUE_OUT_OF_RANGE;    
                }
                break;
                
            case CMD_THR:
                theDataPtr->thr = atoi(command);    
                if( theDataPtr->thr > 2000 || theDataPtr->thr < 1000 ){
                    theDataPtr->command = THROTTLE_VALUE_OUT_OF_RANGE;    
                }
                break;
                
            case CMD_TIME:
                theDataPtr->time = atoi(command);    
                break;
                
            default:
                if (DEBUG_SERIAL) {
                    Serial.println("Too many values in command");
                }
                theDataPtr->command = TOO_MANY_VALUES_IN_COMMAND;    
            }
            
            // Get the next substring from the input string
            // changing the first argument from cmdBuf to 0 is the strtok method for subsequent calls
            command = strtok(0, ",");
            cmd_cnt++;
        }
    }
    theDataPtr->time = millis();
}


void setThrottle(int ch_data) {
  int thr;
  int DIR;
  thr = ch_data;
  //map the channel data to throttle data
  if (ch_data > 1000 && ch_data < 1520) { //reverse
    thr = map(ch_data, 1000, 1520, 255, 0 );
    DIR = LOW;
  } else if (ch_data > 1550 && ch_data < 2000) { //forward
    thr = map(ch_data, 1551, 2000, 0, 255  );
    DIR = HIGH;
  }
  else {
    thr = 0; //stop
    DIR = LOW;
  }

  //shoot through protection
  if ( DIR != PREV_DIR) {
    delay(SHOOT_DELAY);
  }
  PREV_DIR = DIR;

  digitalWrite(PIN_M1_DIR, DIR);
  digitalWrite(PIN_M2_DIR, DIR);
  SoftPWMServoPWMWrite(PIN_M1_PWM, thr); //these aren't servos use pwm
  SoftPWMServoPWMWrite(PIN_M2_PWM, thr);//these aren't servos use pwm
  if (DEBUG_SERIAL) {
    Serial.printf("thr: ch: %d, dir: %d, pwm: %d\n ", ch_data, DIR, thr);
  }
}

void setSteering(int ch_data) {
  int pos = ch_data;
  //map the channel data to steering data
  /*
     ch[1]_str:
     left: 1120
     center: 1523
     right: 1933

     car_str:
     left: 1300: 1400
     center:  1500
     right: 1582: 1633
  */

  SoftPWMServoServoWrite(PIN_STR, pos);
  if (DEBUG_SERIAL) {
    Serial.printf("str: ch: %d servo: %d\n", ch_data, pos);
  }
}


/*
   printIMU to serial port
*/

void sendSerialCommand( commandDataStruct *theDataPtr ){
    Serial.print(theDataPtr->command);
    Serial.print(",");
    Serial.print(theDataPtr->ax);
    Serial.print(",");
    Serial.print(theDataPtr->ay);
    Serial.print(",");
    Serial.print(theDataPtr->az);
    Serial.print(",");
    Serial.print(theDataPtr->gx);
    Serial.print(",");
    Serial.print(theDataPtr->gy);
    Serial.print(",");
    Serial.print(theDataPtr->gz);
    Serial.print(",");
    Serial.print(theDataPtr->time);
    Serial.print(",");
    Serial.print(theDataPtr->str);
    Serial.print(",");
    Serial.print(theDataPtr->thr);
    Serial.println();
//    Serial.flush();        // Serial.flush halts program until all characters are sent
}

void setup() {
  Serial.begin(9600);

  IC1CON = 0;
  IC1CONbits.ICM = 1;   // Capture an interrupt on every rising and falling edge
  IC1CONbits.ICTMR = 1; // Set to user Timer2
  IC1CONbits.ON = 1;    // Turn IC1 on

  IC2CON = 0;
  IC2CONbits.ICM = 1;   // Capture an interrupt on every rising and falling edge
  IC2CONbits.ICTMR = 1; // Set to user Timer2
  IC2CONbits.ON = 1;    // Turn IC2 on

  IC3CON = 0;
  IC3CONbits.ICM = 1;   // Capture an interrupt on every rising and falling edge
  IC3CONbits.ICTMR = 1; // Set to user Timer2
  IC3CONbits.ON = 1;    // Turn IC3 on

  IC4CON = 0;
  IC4CONbits.ICM = 1;   // Capture an interrupt on every rising and falling edge
  IC4CONbits.ICTMR = 1; // Set to user Timer2
  IC4CONbits.ON = 1;    // Turn IC4 on

  /*We're using timer2 for the input capture. This shouldn't interfere with pwm
    output, which uses timers 3-5.
  */
  PR2 = 0xFFFF;   // This tells timer 2 to count up to 0xFFFF, after which it will restart at 0
  T2CONbits.TCKPS = 6;  // 1:64 prescale, which means 80MHz/64 or 1.25MHz clock rate
  T2CONbits.TON = 1;    // Turn on Timer2

  pinMode(RC_INPUT_STR, INPUT);
  pinMode(RC_INPUT_THR, INPUT);
  pinMode(RC_INPUT_KILL, INPUT);
  pinMode(RC_INPUT_MAN, INPUT);

        //these lines set up the interrupt functions to trigger 
  setIntVector(_INPUT_CAPTURE_1_VECTOR, InputCaptureSTR_ISR);
  setIntPriority(_INPUT_CAPTURE_1_VECTOR, 4, 0);
  clearIntFlag(_INPUT_CAPTURE_1_IRQ);
  setIntEnable(_INPUT_CAPTURE_1_IRQ);

  setIntVector(_INPUT_CAPTURE_2_VECTOR, InputCaptureTHR_ISR);
  setIntPriority(_INPUT_CAPTURE_2_VECTOR, 4, 0);
  clearIntFlag(_INPUT_CAPTURE_2_IRQ);
  setIntEnable(_INPUT_CAPTURE_2_IRQ);

  setIntVector(_INPUT_CAPTURE_3_VECTOR, InputCaptureKILL_ISR);
  setIntPriority(_INPUT_CAPTURE_3_VECTOR, 4, 0);
  clearIntFlag(_INPUT_CAPTURE_3_IRQ);
  setIntEnable(_INPUT_CAPTURE_3_IRQ);
    
  setIntVector(_INPUT_CAPTURE_4_VECTOR, InputCaptureMAN_ISR);
  setIntPriority(_INPUT_CAPTURE_4_VECTOR, 4, 0);
  clearIntFlag(_INPUT_CAPTURE_4_IRQ);
  setIntEnable(_INPUT_CAPTURE_4_IRQ);

  pinMode(PIN_M1_DIR, OUTPUT); // Motor1 DIR PIN
  pinMode(PIN_M2_DIR, OUTPUT); //Motor2 DIR PIN
  pinMode(PIN_M1_PWM, OUTPUT); // Motor1 PWM PIN
  pinMode(PIN_M2_PWM, OUTPUT); //Motor2 PWM PIN
  pinMode(PIN_KILL, OUTPUT);
  digitalWrite(PIN_KILL, LOW);
  digitalWrite(PIN_M1_DIR, LOW);
  digitalWrite(PIN_M2_DIR, LOW);
  last_serial_time = millis();
  SoftPWMServoPWMWrite(PIN_M1_PWM, 0);
  SoftPWMServoPWMWrite(PIN_M2_PWM, 0);

  for(int i=0; i<20; i++){
    steer_history[i]=1430;
  }
  steer_next_ind=0;
  //thr_zero_val = pulseIn(PIN_IN_THR, HIGH, 25000);
  for(int i=0; i<20; i++){
      steer_history[i]=1500;
  }
  steer_next_ind=0;
  
  gCenteredSteeringValue = 1500;
  gCenteredThrottleValue = 1500;
    
  gTheOldRCcommand = NOT_ACTUAL_COMMAND;
  gcarState = STATE_MANUAL;//start of in manual (rc control) mode
  
}

void loop() {
// ------------------------- Handle RC Commands -------------------------------
    //we create three commandDataStructs, one each for RC and Serial input, and 
    //one for output
    delay(10);
    commandDataStruct RCInputData, SerialInputData, SerialOutputData;
    RCInputData.command=NOT_ACTUAL_COMMAND;
    SerialInputData.command=NOT_ACTUAL_COMMAND;
    SerialOutputData.command=NOT_ACTUAL_COMMAND;
    SerialInputData.thr=0;
    SerialInputData.str=0;
        
    handleRCSignals( &RCInputData );
    getSerialCommandIfAvailable( &SerialInputData );

    //if this variable is true by the end of loop, we send a serial frame
    bool transmitData=false;

    switch (gcarState){
    case STATE_TERM_AUTO: 
        //if we are in this state, we recieved an RC stop signal, and we're waiting
        //for an ack from the pi so we can stop sending the stop command. All we care
        //about is getting the ack, so we don't even need to check the RC input
        if (SerialInputData.command==STOPPED_AUTO_COMMAND_RECEIVED){
            //if we get the ack from the pi, we can return to manual mode
            gcarState=STATE_MANUAL;
        }else{
            //otherwise, send the stop command again
            SerialOutputData.command=RC_SIGNALED_STOP_AUTONOMOUS;
            transmitData=true;
	}
	break;
    case STATE_AUTONOMOUS:
        Serial.println("AUTONOMOUS MODE");
        //autonomous state-- while in this state, we have to check for stop auto 
        //commands from serial or RC. The only things we check for are RUN_AUTONOMOUSLY
        //and STOP_AUTONOMOUS commands from the Pi, and RC_SIGNALED_STOP_AUTONOMOUS commands
        //from the remote. 
        if (RCInputData.command==RC_SIGNALED_STOP_AUTONOMOUS){ 
            //we could also check for signal_lost signal here, but I don't think that will occur
            SerialOutputData.command=RC_SIGNALED_STOP_AUTONOMOUS;
            SerialOutputData.str = gCenteredSteeringValue;    //  center the steering
	    SerialOutputData.thr = gCenteredThrottleValue;    //  turn off the motor
            setSteering(SerialOutputData.str);
            setThrottle(SerialOutputData.thr);
            transmitData=true;
            gcarState=STATE_TERM_AUTO;
	}else if(SerialInputData.command==STOP_AUTONOMOUS){
            //we're only sending the ack once, so the Pi might miss it and send more 
            //STOP_AUTO commands. So we have to be sure to check for them in manual state
            //and send more stopped_auto acks
            SerialOutputData.command=STOPPED_AUTO_COMMAND_RECEIVED; 
            SerialOutputData.str = gCenteredSteeringValue;    //  center the steering
	    SerialOutputData.thr = gCenteredThrottleValue;    //  turn off the motor
            setSteering(SerialOutputData.str);
            setThrottle(SerialOutputData.thr);
            transmitData=true;
            gcarState=STATE_MANUAL;
        }else if(SerialInputData.command==RUN_AUTONOMOUSLY){
            //we have a new autonomous command -- execute it
            SerialOutputData.str = SerialInputData.str;   //put received command in 
	    SerialOutputData.thr = SerialInputData.thr;   //output to echo back
            setSteering(SerialOutputData.str);
            if (manual_speed==true){
              setThrottle(RCInputData.thr);
            }else{
              setThrottle(SerialOutputData.thr);
            }
	    SerialOutputData.command = GOOD_PI_COMMAND_RECEIVED; 
            transmitData=true;
	}
        break;
    case STATE_MANUAL:
        Serial.println("MANUAL MODE");
        //manual RC state -- while in this state, we send back data frames with the RC signals
        //we also observe for run_auto commands from the Pi and stop_auto commands from the Pi. 
        //Receiving the latter while we're in manual means the Pi missed the stopped_auto ack, 
        //so we should send another.
        if(SerialInputData.command==RUN_AUTONOMOUSLY){
            SerialOutputData.str = SerialInputData.str;   //put received command in 
	    SerialOutputData.thr = SerialInputData.thr;   //output to echo back
            setSteering(SerialOutputData.str);
            setThrottle(SerialOutputData.thr);
	    SerialOutputData.command = GOOD_PI_COMMAND_RECEIVED; 
            transmitData=true;
            gcarState=STATE_AUTONOMOUS;
        }else if(SerialInputData.command==STOP_AUTONOMOUS){
            //it's kind of clumsy to expect to handle this. Really, we should improve the 
            //serial processing on the Pi so that we don't have to worry about commands or 
            //acks being missed. Hopefully this is a temporary solution.
            //The car should respond to RC signals anyway.
            SerialOutputData.command=STOPPED_AUTO_COMMAND_RECEIVED; 
            SerialOutputData.str = RCInputData.str;   
	    SerialOutputData.thr = RCInputData.thr;   
            setSteering(SerialOutputData.str);
            setThrottle(SerialOutputData.thr);
            transmitData=true;
	}else if(RCInputData.command==GOOD_RC_SIGNALS_RECEIVED){
            //This is what we want to happen during manual mode.
            SerialOutputData.str = RCInputData.str;   
	    SerialOutputData.thr = RCInputData.thr;   
            setSteering(SerialOutputData.str);
            setThrottle(SerialOutputData.thr);
            SerialOutputData.command=GOOD_RC_SIGNALS_RECEIVED;
            transmitData=true;
        }
        break;
    }

    if (transmitData==true){
       SerialOutputData.time=SerialInputData.time; //populate time field
       //in the future, the imu will work. The values to send back will
       // always be the ones recorded in RCInputData
       SerialOutputData.ax=RCInputData.ax;
       SerialOutputData.ay=RCInputData.ay;
       SerialOutputData.az=RCInputData.az;
       SerialOutputData.gx=RCInputData.gx;
       SerialOutputData.gy=RCInputData.gy;
       SerialOutputData.gz=RCInputData.gz;
       sendSerialCommand(&SerialOutputData);
    }
}

