#include <SdFat.h>
#include "Adafruit_MAX31855.h"
#include <LiquidCrystal.h>
#include <CapacitiveSensor.h>

int thermoCLK = A4;
int thermoCS = A3;
int thermoDO = A5;

#define relayPin 3
#define thresh 2500 //capacitive touch detect threshold

CapacitiveSensor cs_up = CapacitiveSensor(5,13); 
CapacitiveSensor cs_dn = CapacitiveSensor(7,11);
CapacitiveSensor cs_sel = CapacitiveSensor(A2,A1);
byte capButtonStat[3]={0,0,0};

// Initialize the Thermocouple
Adafruit_MAX31855 thermocouple(thermoCLK, thermoCS, thermoDO);

// initialize the LCD library with the numbers of the interface pins
/*
RS= 6
E=  8
DB4=9
DB5=10
DB6=4
DB7=12
*/
LiquidCrystal lcd(6,8,9,10,4,12);

SdFat sdcard;
SdFile SDfile;  

#define targetTime 210 //Maximum time allowed (seconds)
#define targetTemp 255 //Maximum temperature (Celcius)

//unsigned long instructionExpire=0;
unsigned long currTime;
unsigned long startTime;
unsigned long timers[3] = {0,0,0};
char pFileName[13] = "ProfTest.txt";

void setup() {
  pinMode(relayPin, OUTPUT);
  
  Serial.begin(9600);  
  lcd.begin(8, 2);

  pinMode(3,OUTPUT);
  pinMode(4,OUTPUT);
  
  if(!sdcard.begin(SS, SPI_HALF_SPEED)){
    Serial.println("SD initialization failed!");
    sdcard.initErrorHalt();
    }
  
  updateLCD("Start =", "Select");
  

  while(checkcapbutton(2) == 0){
    }

  startTime = millis();

  sdRWfile('w', &pFileName[0]);  // Open file
  char tbuff[]="Time, Temperature";
  sdRW(&tbuff[0],strlen(tbuff));
  
  digitalWrite(relayPin,HIGH);
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
byte sdRWfile(char op, char *fileNm){

  if(op=='r'){
    if(!SDfile.open(fileNm, O_READ)){
      Serial.println("opening file for reading failed");
      return(0);
      }
    }  
    
  else if(op=='w'){
    if(!SDfile.open(fileNm, O_RDWR | O_CREAT | O_TRUNC)){
      Serial.println("opening file for writing failed");
      return(0);
      }
    }  

  else if(op=='c'){
    SDfile.close();
    }
  return(1);
  }

//////////////////////////////////////////////
void sdRW(char *buff, byte memlength){
  byte fctr=0;

  for(fctr=0; fctr < memlength; fctr++){
    SDfile.print(char(buff[fctr]));
    }
  SDfile.print(char(10));
  SDfile.print(char(13));
  }

///////////////////////////////////////////////
void calcTime(char *time){
  byte strSize;
  double tdiff = (currTime - startTime) / 1000;
  if(tdiff > 99)
    strSize=3;
  else if(tdiff > 9)
    strSize=2;
  else
    strSize=1;
    
  dtostrf(tdiff,strSize,0,&time[0]);
  }

///////////////////////////////////////////////
void updateDisplay(void){
  
  double c = thermocouple.readCelsius();
  char c_str[7];
  dtostrf(c,5,1,&c_str[0]);

  char disp[17];
  char Ttime[4]="";

  strcpy(disp,"TIME:");
  calcTime(Ttime);
  strcat(disp,Ttime);

  updateLCD(disp, c_str);
  }


//////////////////////////////////////////////
void saveTemp(void){
  char time_str[4]="";

  char timetemp[7]="";
  calcTime(&time_str[0]);

  double c = thermocouple.readCelsius();
  char c_str[7];
  dtostrf(c,5,1,&c_str[0]);  
  
  strcat(timetemp,time_str);
  strcat(timetemp,",");
  strcat(timetemp,c_str);

  sdRW(&timetemp[0],strlen(timetemp));  
  
  }

//////////////////////////////////////////////

void loop() {

  currTime = millis();

  if(currTime > timers[1]){
    timers[1] = currTime + 10000;
    saveTemp();
    }

  if(currTime > timers[0]){
    timers[0] = currTime + 1000;
    updateDisplay();
    }

  if(currTime > timers[2]){
    timers[2] = currTime + 1000;
    if( (currTime - startTime)/1000 > targetTime ||
        thermocouple.readCelsius() > targetTemp){
      saveTemp();
      digitalWrite(relayPin,LOW);
      updateLCD("Profile", "Done");
      sdRWfile('c', &pFileName[0]);  // Open file
      while(1);
      }
    }
  }
  
