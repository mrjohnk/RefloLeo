#include <SdFat.h>
#include "Adafruit_MAX31855.h"
#include <LiquidCrystal.h>
#include <CapacitiveSensor.h>

int thermoCLK = A4; //F1
int thermoCS = A3;  //F4
int thermoDO = A5;  //F0

#define relayPin 11
#define relayAux 12
#define thresh 2500 //capacitive touch detect threshold

CapacitiveSensor cs_up = CapacitiveSensor(13,A9);  // C7,B5 (32,29)
CapacitiveSensor cs_dn = CapacitiveSensor(A2,7);   // F5,E6 (38,1)
CapacitiveSensor cs_sel = CapacitiveSensor(A0,A1); // F7,F6 (36,37)
byte capButtonStat[3]={0,0,0};

// Initialize the Thermocouple
Adafruit_MAX31855 thermocouple(thermoCLK, thermoCS, thermoDO);

// initialize the LCD library with the numbers of the interface pins
/*
RS=A7    // (D7 27)
E=A8    //  (B4 28)
DB4=5   //  (C6 31)
DB5=A10 //  (B6 30)
DB6=1   //  (D3 21)
DB7=A6  //  (D4 25)
*/
LiquidCrystal lcd(A7,A8,5,A10,1,A6);

SdFat sdcard;
SdFile SDfile;  

char currInstruction = 'O';
unsigned long currTime;
unsigned long timers[2] = {0,0};
byte instructionStatus = 0;
unsigned long instructionExpire;
char parameter = ' ';
char v1_str[4];
char v2_str[4];
long v1_int;
long v2_int;
int selectedprofile = -1;

void setup() {
  pinMode(relayPin, OUTPUT);
  pinMode(relayAux, OUTPUT);
  
  Serial.begin(9600);  
  lcd.begin(8, 2);

  if(!sdcard.begin(SS, SPI_HALF_SPEED)){
    Serial.println("SD initialization failed!");
    sdcard.initErrorHalt();
    }
  
  updateLCD("Select P", "rofile:0");
 
 Serial.println("getting profile");
  
  while(selectedprofile < 0){
    selectedprofile = getProfile();
    if(selectedprofile >= 0){
      Serial.println("checking profile");
      if(checkProfile() == 0)
        selectedprofile = -1;
        }
      }

 Serial.println("opening file");

  sdRWfile('o',selectedprofile);  // Open file

 Serial.println("going to main loop");

} 

////////////////////////////////////////////
byte checkProfile(void){
  char tempc[5]={'\0','\0','\0','\0','\0'};
  char fileName[10]={'\0','\0','\0','\0','\0','\0','\0','\0','\0','\0'};
  itoa(selectedprofile,tempc,10);
  strcat(fileName,tempc);
  strcat(fileName,".txt");
  if( sdcard.exists(fileName) )
    return(1);
  else{
    updateLCD("File Not","Exist");
    delay(3000);
    updateLCD("Select P", "rofile:0");
    return(0);
    }
  }

////////////////////////////////////////////
byte getProfile(void){
  int sp = -1;
  byte displayedProfile=0;

  while(sp < 0){
    byte bctr=0;
    char buff[4];

    for(bctr=0;bctr<3;bctr++){
      byte bstat=checkcapbutton(bctr);
      if(bstat != capButtonStat[bctr]){
        capButtonStat[bctr]=bstat;
        if(bstat==1){
          switch(bctr){
            case 0:
              displayedProfile++;
              itoa(displayedProfile, buff, 10);
              updateLCD("Profile:", buff);
              Serial.println("up");
              break;
            case 1:
              displayedProfile--;
              itoa(displayedProfile, buff, 10);
              updateLCD("Profile:", buff);
              Serial.println("down");
              break;
            case 2:
              sp = displayedProfile;
              Serial.println("select");
              break;
          }
        }
      }
    } 
  }
  return(sp);
  }

////////////////////////////////////////////
byte checkcapbutton(byte bnum){
  long tstat=0;
  switch(bnum){
    case 0:
      tstat=cs_up.capacitiveSensor(30);
      break;
    case 1:
      tstat=cs_dn.capacitiveSensor(30);
      break;
    case 2:
      tstat=cs_sel.capacitiveSensor(30);
      break;
    }
  if(tstat > thresh)
    return(1);
  else
    return(0);
  }

////////////////////////////////////////////
void updateLCD(char *s1, char *s2){
  char firstline[9] = {' ',' ',' ',' ',' ',' ',' ',' ','\0'};
  char secline[9] = {' ',' ',' ',' ',' ',' ',' ',' ','\0'};

  //transfer first string into first line (left justify)
  byte ele=0;
  while(s1[ele] != '\0' && ele < 10){
    firstline[ele] = s1[ele];
    ele++;
    }

  //measure size of second string
  ele=0;
  while(s2[ele] != '\0' && ele < 10){
    ele++;
    }

  //transfer second string with offset (right justify)
  byte rjctr = 8 - ele;
  byte ectr;
  for(ectr = rjctr; ectr < 8; ectr++)
    secline[ectr] = s2[ectr - rjctr];
    
  lcd.setCursor(0, 0);
  lcd.print(firstline);
  lcd.setCursor(0, 1);  
  lcd.print(secline);
  }

////////////////////////////////////////////
byte sdRWfile(char op, byte fileNm){

  if(op=='o'){
    char fileName[10]={'\0','\0','\0','\0','\0','\0','\0','\0','\0','\0'};
    char tempc[4];
    itoa(fileNm,tempc,10);
    strcat(fileName,tempc);
    strcat(fileName,".txt");
    if(!SDfile.open(fileName, O_READ)){
      Serial.println("opening file for reading failed");
      return(0);
      }
    }  
  else if(op=='c'){
    SDfile.close();
    }
  return(1);
  }

//////////////////////////////////////////////
byte sdRW(char *buff){
  byte fctr=0;
  byte gotchar;

  for(fctr=0;fctr<25;fctr++){
    gotchar = SDfile.read();
    if(gotchar != 10 && gotchar != 13)
      buff[fctr] = char(gotchar);
    if(gotchar == 13){
      buff[fctr]='\0';
      break;   //stop reading at end of line <CR>
      }
    if(gotchar == 10)
      fctr--;
    }
  if(fctr>=25) return(0);
  else return(fctr);
  }

//////////////////////////////////////////////
byte checkHold(void){
  if(currTime > instructionExpire){
    instructionStatus = 0;
    }
  else{
    int c = thermocouple.readCelsius();
    int delta = v1_int - c;
    if(delta > 0)
      digitalWrite(relayPin,HIGH);
    else
      digitalWrite(relayPin,LOW);
    }
  }

//////////////////////////////////////////////
byte checkTemp(void){
  int c = thermocouple.readCelsius();
  int delta = c - v1_int;
  if(delta >= 0){
    instructionStatus = 0;
    }
  else{
    digitalWrite(relayPin,HIGH);
    }
  }

//////////////////////////////////////////////
byte checkWait(void){
  digitalWrite(relayPin,LOW);
  if(currTime > instructionExpire)
    instructionStatus = 0;
  }

///////////////////////////////////////////////
void getInstruction(void){
  char instruction[9] = "";
  sdRW(instruction);

  byte v1_offset=1;
  if(instruction[1] == '0')
    v1_offset=2;

  byte vctr=v1_offset - 1;
  for(vctr=0;vctr<3+1-v1_offset;vctr++){
    v1_str[vctr]=instruction[vctr+v1_offset];
    }
  v1_str[vctr]='\0';

  byte v2_offset=5;
  if(instruction[5] == '0')
    v2_offset=6;

  vctr=v2_offset - 5;
  for(vctr=0;vctr<3+5-v2_offset;vctr++){
    v2_str[vctr]=instruction[vctr+v2_offset];
    }
  v2_str[vctr]='\0';

  v1_int = atoi(v1_str);
  v2_int = atoi(v2_str);

  currInstruction = instruction[0];

  if(instruction[0]=='H' || instruction[0]=='W'){ 
    Serial.println(v2_int);
    instructionExpire = currTime + (v2_int * 1000);
    }

  instructionStatus=1;
  }

///////////////////////////////////////////////
void calcTimeLeft(char *timeLeft){
  byte strSize;
  double tdiff = (instructionExpire - currTime) / 1000;
  if(tdiff > 99)
    strSize=3;
  else if(tdiff > 9)
    strSize=2;
  else
    strSize=1;
    
  dtostrf(tdiff,strSize,0,&timeLeft[0]);
  }

///////////////////////////////////////////////
void updateDisplay(void){
  char dispInstruction[17];
  char timeleft[4]="";

  if(currInstruction == 'H'){
    strcpy(dispInstruction,"HOLD:");
    strcat(dispInstruction,v1_str);
    //calcTimeLeft(timeleft);
    //strcat(dispInstruction,timeleft);
    }
  else if(currInstruction == 'T'){
    strcpy(dispInstruction,"GOTO:");
    strcat(dispInstruction,v1_str);
    }
  else if(currInstruction == 'W'){
    strcpy(dispInstruction,"WAIT:");
    calcTimeLeft(timeleft);
    strcat(dispInstruction,timeleft);
    //strcat(dispInstruction,v2_str);
    }
  else if(currInstruction == 'X')
    strcpy(dispInstruction,"EXIT");
    
  double c = thermocouple.readCelsius();
  char c_str[7];
  dtostrf(c,5,1,&c_str[0]);

  Serial.print("Temp=");
  Serial.println(c);

  updateLCD(dispInstruction, c_str);
  }

///////////////////////////////////////////////
void checkExit(void){
  instructionStatus=0;
  sdRWfile('c',selectedprofile);    
  digitalWrite(relayPin,LOW);
  }

///////////////// Main Instruction Set ////////////////////////////// 
/*
T = Heat to specified temperature
H = Hold above specified temperature
W = Wait specified amount of time (in seconds)
X = Exit program (close file, deactivate heat)

Time and temperature always given in 3 digit format

Example profile (don't include spaces or text at right below):
T130,000          Goto 130 degrees
W000,015          Wait 15 seconds (and turn off heat)
H150,120          Hold at 150 degrees for 120 seconds
T212,000          Goto 212 degrees
W000,020          Wait 20 seconds (and turn off heat)
H225,020          Hold at 225 degrees for 20 seconds
X000,000          Exit program and turn off relay

Timers:
 0 = update diaplay
 1 = when to visit current instruction again for update
*/

void loop() {

  currTime = millis();
  
  //if there is no active command, read another from SD card
  if(instructionStatus == 0 && currInstruction != 'X')
    getInstruction();
  
  if(currTime > timers[1] && instructionStatus == 1){
    if(currInstruction=='T') {
      checkTemp();
      timers[1] = currTime + 1000;
      }
    else if(currInstruction=='H'){
      checkHold();
      timers[1] = currTime + 5000;
      }
    else if(currInstruction=='W'){
      checkWait();
      timers[1] = currTime + 1000;
      }
    else if(currInstruction=='X'){
      checkExit();
      }
    }

  if(currTime > timers[0]){
    timers[0] = currTime + 1000;
    updateDisplay();
    }
  }
  
