#include <SdFat.h>
#include "Adafruit_MAX31855.h"
#include <LiquidCrystal.h>
#include <CapacitiveSensor.h>

// NEW FEATURES/BUG FIXES from -mpogue:
//
// Banner message: "RefloLeo, etc" with version number
// At boot, if you forgot to plug in an SD card, we will wait for you to do so, and then go on.
// Multiple profile cycles are allowed, without having to power down the board each time.  When
//   a profile is done (or cancelled), the display goes back to that profile, to choose again.
// You can cancel a running profile via the [] button (NOTE: must press a second time to confirm --
//    if you don't confirm within 5 seconds, the cancellation will be aborted, and we'll
//    go back to run state)
// Comments are allowed in profiles, using the "#" character as the first character on the line
// When choosing a profile, the first line in the file (usually a comment) is shown to you.
//   A good use for this: put "# Pb-free profile" or "# R276 profile", etc. as the first line
//   of a profile, to remind you which profile is which.
// When choosing a profile, you can only choose profiles that actually exist (note that
//   profile numbers must be contiguous, starting at zero).
// Added a "D" command, which means TELL ME TO OPEN THE DOOR (with flashing blue light
//   on Relay2), until the temp goes down below a certain value, then continue.  Generally
//   followed by an X command, indicating end-of-cycle.
// Spaces are allowed in temp and time specifiers, e.g. T100, 10
// Single digit temp and time specifiers are allowed, too, e.g. "H9,0"
// Logs show one time/temp entry per second (rather than 10 seconds), so we can't
//    miss the peak temp (some toaster ovens change temp pretty quick).
// Added a header to the log file (easier processing with Excel, R, or Python later)
// Logfiles use standard UNIX EOL (NL), rather than Windows-specific EOL (CR/NL)
// Display shows temp followed by a "degrees" symbol and a "C"
//   (used http://assets.devx.com/articlefigs/16587.png as a reference for characters)
// Simplified the timers (fewer of them now, don't have to worry about display temp
//   being different from actual temp).
// Serial.print*() replaced with DEBUG and DEBUGLN, so that all debugging
//   statements can be removed or added back in with a single #define
// Changed most magic numbers to constants, to make the code clearer (and smaller).
// Made indentation consistent throughout using astyle (*which* style is best a matter of 
//   taste, I suppose -- I chose 1TBS style, as per the UNIX and LINUX kernel code.  I think 
//   this is one of the most common and readable styles in use today, although it's not my 
//   personal favorite...).
// Added a bunch of comments, to help the next hacker!
//
// TODO: better scanning of a profile before allowing it to be chosen.  Right now, it is just
//   a check that the file exists.
// TODO: the SD card can be inserted after boot, but after that, if the SDCARD is pulled out,
//   we can't read it again.  It would be nice to be able to remove and reinsert at any time
//   (when not running a profile).
// TODO: ideally, we should propbably split sdRWfile and sdRW into separate functions, so that 
//   input and output file concerns are handled separately.
// TODO: might be able to get rid of the 'X' command entirely, since it is always implied by the EOF.

// Relay control
#define relayPin 11
#define relayAux 3

// Capacitive buttons
CapacitiveSensor cs_up = CapacitiveSensor(13, A9); // C7,B5 (32,29)
CapacitiveSensor cs_dn = CapacitiveSensor(A2, 7);  // F5,E6 (38,1)
CapacitiveSensor cs_sel = CapacitiveSensor(A0, A1); // F7,F6 (36,37)
byte capButtonStat[3] = {0, 0, 0};

#define thresh 2200 // capacitive touch detect threshold

const byte kUPBUTTON = 0;
const byte kDOWNBUTTON = 1;
const byte kSQUAREBUTTON = 2;

// Thermocouple
const int thermoCLK = A4; // F1
const int thermoCS = A3;  // F4
const int thermoDO = A5;  // F0

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
LiquidCrystal lcd(A7, A8, 5, A10, 1, A6);

// SD card
SdFat sdcard;
SdFile SDfile;
SdFile SDfileLog;

// Master state machine state
byte currentState = 1;
const byte kGetProfile = 1;
const byte kRunningProfile = 2;
const byte kFirstCancelDown = 3;
const byte kCancelling = 4;
const byte kSecondCancelDown = 5;

// Timers:
const byte kINSTRUCTIONTIMER = 0;  // display, instruction, and log timers (unified)
const byte kTEMPTIMER = 1;         // time to get a temp reading?
const byte kCANCELTIMER = 2;       // time to give up on cancelling?
unsigned long timers[3] = {0, 0, 0};

const int kCANCELTIMEOUT = 5000;  // 5 seconds to confirm CANCEL, or we revert to run state

// instruction fetching
char currInstruction = 'I';  // _I_nitial state
unsigned long currTime;      // in milliseconds

byte instructionStatus = 0;
unsigned long instructionExpire;
double currTemp = 0;
//char parameter = ' ';
char v1_str[4];  // first command variable, as string
char v2_str[4];  // second command variable, as string
long v1_int;     // first command var, as int
long v2_int;     // second command var, as int

const byte kMAXLINELENGTH = 25;  // max line length in the Profile files

const signed int kNOPROFILESELECTED = -1;
const signed int kPROFILEOK = 1;
const signed int kPROFILEBAD = 0;
int selectedprofile = kNOPROFILESELECTED;
byte displayedProfile = 0;

// Log file state
const byte kLOGFILECLOSED = 0;
const byte kLOGFILEOPEN = 1;
const byte kLOGFILEOPENABOVE125 = 2;  // entries have reached 125 degrees C, auto-close logfile on the way down
byte logFileStat = kLOGFILECLOSED;

unsigned int log_time_in_seconds = 0; // for recording time in logfile

// display and LED flashing
byte flashRELAY2 = 0;   // when cycle is complete, flash the RELAY2 LED once per second
int d_Flasher = -1;     // used for displaying 2 messages alternately in DOOR OPEN state

// constants for parsing characters from lines in the the input file
const byte kSPACE = 32;
const byte kCR = 13;
const byte kNL = 10;
const byte kLowerCaseZ = 122;
const char kCOMMA = ',';
const char kZERO = '0';     // the character zero
const byte kNULL = 0;       // sometimes files can end in a NULL with no CR or NL

// turn on debug print statements, by uncommenting the following line...
//#define DEBUG_ME
#ifdef DEBUG_ME
#define DEBUG(x) Serial.print(x);
#define DEBUGLN(x) Serial.println(x);
#else
#define DEBUG(x)
#define DEBUGLN(x)
#endif

/////////////////////////////////////////////////
// This stuff is only executed once per board boot.
void setup(void)
{
    pinMode(relayPin, OUTPUT);
    pinMode(relayAux, OUTPUT);

    Serial.begin(9600);

    lcd.begin(16, 2);

    // Welcome message
    updateLCD("RefloLeo Reflow", "Controller V1.4");
    delay(1000);

    // if there's no SD card found, keep trying
    while (!sdcard.begin(SS, SPI_HALF_SPEED)) {
        DEBUGLN("SD initialization failed!");
        DEBUGLN("No SD Memory Card Detected ");
        updateLCD("Insert SD Card", " ");
        delay(1000);  // try once a second, until we find an SD card...
    }
    DEBUGLN("SD Memory Card Detected...");

    if ( sdcard.exists("selftest") ) {
        selfTest();
    }

    // state machine will start up in kGetProfile state, asking user to choose a profile..
    currentState = kGetProfile;

} // setup

////////////////////////////////////////////
void selfTest(void)
{
    if (SDfile.open("selftest", O_WRITE)) {
        updateLCD("Open selftest", "SUCCESS");
    }
    else {
        updateLCD("Open selftest", "FAILED");
    }

    delay(3000);

    byte fets[2] = {0, 128};

    while (checkcapbutton(kSQUAREBUTTON) == 0) {
        double c = thermocouple.readCelsius();
        char c_str[7];
        dtostrf(c, 5, 1, &c_str[0]);

        char disp[17] = "Temp:";
        strcat(disp, c_str);

        if (checkcapbutton(kUPBUTTON)) {
            strcat(disp, " UP");
        }

        if (checkcapbutton(kDOWNBUTTON)) {
            strcat(disp, " DOWN");
        }

        updateLCD("Running-SelfTest", disp);

        analogWrite(relayPin, fets[0] += 6);
        analogWrite(relayAux, fets[1] += 6);
    }

    if (SDfile.remove()) {
        updateLCD("Remove selftest", "SUCCESS");
    }
    else {
        updateLCD("Remove selftest", "FAILED");
    }

    while (1);
}

////////////////////////////////////////////
// returns true iff profile p.txt exists (where p is an 8-bit integer)
bool checkProfileExists(byte p)
{
    char tempc[5] = {'\0', '\0', '\0', '\0', '\0'};
    char fileName[10] = {'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
    itoa(p, tempc, 10);
    strcat(fileName, tempc);
    strcat(fileName, ".txt");

    return (sdcard.exists(fileName));
}

////////////////////////////////////////////
// gets the first line of the file b.txt, where b is an integer,
//   into the string s.  Note: the file is assumed to exist.
void getFirstLine(byte b, char *s)
{
    // Open profile spec file for input, e.g. "1.txt"
    sdRWfile('o', (int)b, 'i');

    // Read one line into buffer s
    sdRW(s, 'i', 0);

    // Close the profile spec file for input, e.g. "1.txt"
    sdRWfile('c', (int)b, 'i');
}

// given a profile number, puts up "Run profile: #"
//   and the first line of the #.txt file, into the LCD display.
void showProfile(byte d)
{
    char firstLine[kMAXLINELENGTH];
    char buff[4];
    char disp[17] = "Run profile: ";

    itoa(d, buff, 10);
    strcat(disp, buff); // e.g. "Run profile: 2", note only 2 digits max allowed here

    getFirstLine(d, firstLine);  // get the first line of the file <displayedProfile>.txt

    updateLCD(disp, firstLine);
    displayedProfile = d;        // global
}

////////////////////////////////////////////
byte getProfile(void)
{
    int sp = -1;

    while (sp < 0) {
        byte bctr = 0;

        for (bctr = 0; bctr < 3; bctr++) {
            byte bstat = checkcapbutton(bctr);
            if (bstat != capButtonStat[bctr]) {
                capButtonStat[bctr] = bstat;
                if (bstat == 1) {
                    switch (bctr) {
                        case 0:
                            if (checkProfileExists(displayedProfile + 1)) { // can only choose profiles that exist!
                                displayedProfile++;
                                showProfile(displayedProfile);
                            }
                            break;
                        case 1:
                            if (checkProfileExists(displayedProfile - 1)) { // can only choose profiles that exist!
                                displayedProfile--;
                                showProfile(displayedProfile);
                            }
                            break;
                        case 2:
                            sp = displayedProfile;
                            break;
                    } // switch
                } // if
            } // if
        } // for
    } // while
    return (sp);
}

////////////////////////////////////////////
byte checkcapbutton(byte bnum)
{
    long tstat = 0;
    switch (bnum) {
        case kUPBUTTON:
            tstat = cs_up.capacitiveSensor(30);
            break;
        case kDOWNBUTTON:
            tstat = cs_dn.capacitiveSensor(30);
            break;
        case kSQUAREBUTTON:
            tstat = cs_sel.capacitiveSensor(30);
            break;
    }
    if (tstat > thresh) {
        return (1);
    }
    else {
        return (0);
    }
}

////////////////////////////////////////////
void updateLCD(char *s1, char *s2)
{
    char firstline[17] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '\0'};
    char secline[17] =   {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', '\0'};

    byte ele = 0;
    while (s1[ele] != '\0' && ele < 18) {
        firstline[ele] = s1[ele];
        ele++;
    }

    ele = 0;
    while (s2[ele] != '\0' && ele < 18) {
        secline[ele] = s2[ele];
        ele++;
    }

    lcd.setCursor(0, 0);
    lcd.print(firstline);
    lcd.setCursor(0, 1);
    lcd.print(secline);
}

////////////////////////////////////////////
byte sdRWfile(char op, byte fileNm, char ftype)
{

    if (op == 'o' && ftype == 'i') {
        char fileName[10] = {'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
        char tempc[4];
        itoa(fileNm, tempc, 10);
        strcat(fileName, tempc);
        strcat(fileName, ".txt");
        if (!SDfile.open(fileName, O_READ)) {
            DEBUGLN("opening file for reading failed");
            return (0);
        }
    }

    else if (op == 'o' && ftype == 'o') {
        char fileName[10] = {'\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
        char tempc[4];
        itoa(fileNm, tempc, 10);
        strcat(fileName, tempc);
        strcat(fileName, "-out.txt");
        if (!SDfileLog.open(fileName, O_RDWR | O_CREAT | O_TRUNC)) {
            DEBUGLN("opening file for writing failed");
            return (0);
        }

        // write a header at the top of the log file
        char logHeader[] = "time_in_seconds,temp_in_degrees_C";
        sdRW(logHeader, 'o', strlen(logHeader));
    }

    else if (op == 'c' && ftype == 'i') {
        SDfile.close();
    }

    else if (op == 'c' && ftype == 'o') {
        SDfileLog.close();
    }

    return (1);
}

//////////////////////////////////////////////
byte sdRW(char *buff, char op, byte memlength)
{
    byte fctr = 0; // number of bytes that we got

    if (op == 'i') {
        byte gotchar;

        for (fctr = 0; fctr < kMAXLINELENGTH; fctr++) {
            gotchar = SDfile.read();
            // check for valid instruction character
            if (gotchar >= kSPACE && gotchar <= kLowerCaseZ) {
                buff[fctr] = char(gotchar);
            }
            else {
                if (gotchar == kCR) {
                    // read one more character, which should be 10 (LF)
                    gotchar = SDfile.read();
                    if (gotchar != kNL) {
                        // FIX: is this still needed?
                        DEBUG("ERROR: did not get expected LF");  // in this case, will have to save character, etc
                    }
                }
                if (gotchar == kNL || gotchar == kCR || gotchar == 255) {
                    buff[fctr] = '\0';
                    break;
                }

                fctr--;    //non-instruction character; decrement char counter
            } // else
        } // for
        if (fctr >= kMAXLINELENGTH) {
            return (0);
        }
        else {
            return (fctr);
        }
    }

    else if (op == 'o') {
        for (fctr = 0; fctr < memlength; fctr++) {
            SDfileLog.print(char(buff[fctr]));
        }
        // SDfileLog.print(char(10));  // CR, not needed nowadays!
        SDfileLog.print(char(kNL));  // NL, standard on UNIX, LINUX, MAC OS X, readable on Windows
    }
}

//////////////////////////////////////////////
// Hold oven at temp T for N seconds, then go on
byte checkHold(void)
{
    if (currTime > instructionExpire) {
        instructionStatus = 0;
    }
    else {
        int delta = v1_int - currTemp;
        if (delta > 0) {
            digitalWrite(relayPin, HIGH);
        }
        else {
            digitalWrite(relayPin, LOW);
        }
    }
}

//////////////////////////////////////////////
// turn oven ON until temp T is reached, then go on
byte checkTemp(void)
{
    double delta = currTemp - (double)v1_int;

    if (delta >= 0) {
        instructionStatus = 0;  // DONE. get a new instruction
    }
    else {
        digitalWrite(relayPin, HIGH);
    }
}

//////////////////////////////////////////////
// Wait for N seconds with the oven turned OFF, then go on
byte checkWait(void)
{
    digitalWrite(relayPin, LOW);
    if (currTime > instructionExpire) {
        instructionStatus = 0;
    }
}

///////////////////////////////////////////////
// Close the log file, turn OFF the oven, and STOP interpreting instructions.
// (COOLDOWN state)
void checkExit(void)
{
    digitalWrite(relayPin, LOW);           // turn off oven
    digitalWrite(relayAux, LOW);           // turn off LED

    instructionStatus = 0;                 //

    sdRWfile('c', selectedprofile, 'i');   // close input profile file
    DEBUGLN("input file closed");

    if (logFileStat != kLOGFILECLOSED) { // if log file is still open at this point...
        sdRWfile('c', selectedprofile, 'o');  // ...close the log file
        DEBUGLN("output file closed");
    }
}

///////////////////////////////////////////////
// Turn the oven OFF, and wait for the temp to drop below T
void checkDoor(void)
{
    double delta = currTemp - (double)v1_int;
    digitalWrite(relayPin, LOW);           // DOOR OPEN, turn off oven (always)

    if (delta <= 0) {
        digitalWrite(relayAux, LOW);    // turn off LED; the D state always exits with LED off
        instructionStatus = 0;          // temp lower than specified value, so go get next instruction
    }
    else {
        // flash RELAY2 LED, to get our attention to open the door
        if (flashRELAY2++ % 2 == 0) {
            digitalWrite(relayAux, HIGH); // LED ON
        }
        else {
            digitalWrite(relayAux, LOW); // LED OFF
        }
    }
}

///////////////////////////////////////////////
// read the next line from the currently open Profile
byte sdReadProfileLine(char *b)
{
    return (sdRW(b, 'i', 0));
}

void getInstruction(void)
{
    // read in a line from the input file (NOTE: max line length = kMAXLINELENGTH)
    char instruction[kMAXLINELENGTH] = "";

    do {
        sdReadProfileLine(instruction);
    }
    while (instruction[0] == '#');   // skip comment lines

    // # line looks like this: "TXXX,YYY" where XXX and YYY can be 1 to 3-digit integers
    // spaces are disallowed right now
    //

    char *psrc = &instruction[1]; // always skip over the command char, e.g. 'T'
    char *pv1 = v1_str;
    char *pv2 = v2_str;

    bool gotNonZero = false;

    // copy over the v1_str
    do {
        *pv1 = *psrc++;
        if (gotNonZero || (*pv1 != kZERO)) {
            gotNonZero = true;
            pv1++;
        }
    }
    while (*psrc != kCOMMA);

    if (!gotNonZero) { // result was '0'
        pv1++; // so skip over it
    }
    *pv1 = kNULL;

    // pointing to the comma, so point at v2 next
    psrc++;

    // copy over the v2_str
    // Note that if the last line of the file is not well-formed, we might just run out of bytes
    gotNonZero = false;
    do {
        *pv2++ = *psrc++;
        if (gotNonZero || (*pv2 != kZERO)) {
            gotNonZero = true;
            pv2++;
        }
    }
    while (*psrc != kCR && *psrc != kNL && *psrc != kNULL);

    if (!gotNonZero) { // result was '0'
        pv2++;  // point at just beyond the '0'
    }
    *pv2 = kNULL;  // stick a null on the end of v2_str so it's a proper C string

    // convert those 2 strings to actual integers
    v1_int = atoi(v1_str);
    v2_int = atoi(v2_str);

    // instruction is the first byte in the line (spaces not allowed before this)
    currInstruction = instruction[0];

    // Hold and Wait both set the instruction expiration timer
    if (instruction[0] == 'H' || instruction[0] == 'W') {
        instructionExpire = currTime + (v2_int * 1000);
    }

    d_Flasher = -1;  // every instruction resets the flasher, in case it needs to present alternate msgs
    instructionStatus = 1;  // have an instruction
}

///////////////////////////////////////////////
void calcTimeLeft(char *timeLeft)
{
    byte strSize;

    long tdiff = instructionExpire - currTime;
    if (tdiff > 0) {
        tdiff = tdiff / 1000;
    }
    else {
        tdiff = 0;
    }


    if (tdiff > 99) {
        strSize = 3;
    }
    else if (tdiff > 9) {
        strSize = 2;
    }
    else {
        strSize = 1;
    }

    dtostrf(tdiff, strSize, 0, &timeLeft[0]);
}

///////////////////////////////////////////////
void updateDisplay(void)
{

    char LCDline1[17];
    char timeleft[4] = "";
    char degreesC[3] = {0xDF, 'C', 0x00};

    switch (currInstruction) {

        case 'H':
            strcpy(LCDline1, "HOLD:");
            strcat(LCDline1, v1_str);
            strcat(LCDline1, " | ");
            calcTimeLeft(timeleft);
            strcat(LCDline1, timeleft);
            break;
        case 'T':
            strcpy(LCDline1, "GO TO: ");
            strcat(LCDline1, v1_str);
            strcat(LCDline1, degreesC);
            break;
        case 'W':
            strcpy(LCDline1, "WAIT: ");
            calcTimeLeft(timeleft);
            strcat(LCDline1, timeleft);
            break;
        case 'D':
            if (d_Flasher == -1) { // if just starting D state
                d_Flasher = 0;  // make sure we start with the open door message
            }
            if ((d_Flasher++ % 4) <= 1) {
                strcpy(LCDline1, "OPEN DOOR NOW!");  // flash this for 2 seconds (0/1 case)
            }
            else {
                strcpy(LCDline1, "COOL TO: ");       // and this for 2 seconds (2/3 case)
                strcat(LCDline1, v1_str);
                strcat(LCDline1, degreesC);
            }
            break;
        //    case 'X':
        //      strcpy(LCDline1, "COOLING DOWN...");  // never shown anymore...we exit immediately now
        //      break;
        default:
            strcpy(LCDline1, "");  // initial state is 'I', so just clear the display, if this happens
            break;
    }

    char c_str[7];
    dtostrf(currTemp, 5, 1, &c_str[0]);

    DEBUG("Temp=");
    DEBUG(currTemp);
    DEBUG(" | ");
    DEBUGLN(currInstruction);

    char LCDline2[17] = "Temp:";
    strcat(LCDline2, c_str);
    strcat(LCDline2, degreesC);

    updateLCD(LCDline1, LCDline2);
}


///////////////////////////////////////////////
void logTemp(void)
{
    // close log automatically, when temp goes up and then down below 125 degrees C
    if (currTemp < 125 && logFileStat == kLOGFILEOPENABOVE125) {
        sdRWfile('c', selectedprofile, 'o');
        logFileStat = kLOGFILECLOSED;  // log file is CLOSED
    }

    if (logFileStat == kLOGFILEOPEN && currTemp > 125) {
        logFileStat = kLOGFILEOPENABOVE125;  // log file is OPEN and we got over 125 degrees...
    }

    if (logFileStat != kLOGFILECLOSED) { // if log file is OPEN
        char tbuff[10];
        char tempIndexStr[4];

        char c_str[7];
        if (currTemp >= 100) {
            dtostrf(currTemp, 5, 1, &c_str[0]);
        }
        else {
            dtostrf(currTemp, 4, 1, &c_str[0]);
        }

        itoa(log_time_in_seconds, &tempIndexStr[0], 10);

        strcpy(tbuff, tempIndexStr);
        strcat(tbuff, ",");
        strcat(tbuff, c_str);

        sdRW(tbuff, 'o', strlen(tbuff));
        log_time_in_seconds++;
    }
}

void updateTemp(void)
{
    double c = thermocouple.readCelsius();
    if (c > 10 && c < 285) { //filters out erroneous thermocouple readings
        currTemp = c;
    }
}

///////////////// Main Instruction Set //////////////////////////////
/*
T = Heat to specified temperature
H = Hold above specified temperature
W = Wait specified amount of time (in seconds)
D = Deactivate heat, flash OPEN DOOR message and RELAY2 LED, until temp is lower than specified
X = Exit program (close file, deactivate heat)

Time and temperature always given in 3 digit format

Example profile (don't include spaces or text at right below):
T130,000          Goto 130 degrees
W000,015          Wait 15 seconds (and turn off heat)
H150,120          Hold at 150 degrees for 120 seconds
T212,000          Goto 212 degrees
W000,020          Wait 20 seconds (and turn off heat)
H225,020          Hold at 225 degrees for 20 seconds
D150,000          Turn off relay, show OPEN DOOR message and flash LED2, don't go on until temp drops below 150 degrees
X000,000          Exit program and turn off relay, and turn off logging

Timers:
 0 = when to update the display
 1 = when to visit current instruction again for update
 2 = when to write out a log entry
 3 = when to get a thermocouple reading
*/

// run the cycle, but only update the display if I tell you to...
void running(unsigned long currTime, bool doDisplay)
{
    // only update the display, if I told you to...
    // NOTE: cancel states will turn off display update (temporarily)
    // ALSO NOTE: if we don't have a currentInstruction, don't update the display yet

    // first, get the next instruction
    if (instructionStatus == 0 && currInstruction != 'X') {
        getInstruction();
    }

    if (currTime > timers[kINSTRUCTIONTIMER] && instructionStatus == 1) {
        timers[kINSTRUCTIONTIMER] = currTime + 1000;

        // update the LCD display, if we're not at the initial state
        if ( doDisplay && (currInstruction != 'I') ) {
            updateDisplay();
        }

        // Write a log entry
        logTemp();

        // execute an instruction
        switch (currInstruction) {
            case 'T':
                checkTemp();
                break;
            case 'H':
                checkHold();
                break;
            case 'W':
                checkWait();
                break;
            case 'D':
                checkDoor();
                break;
            // case 'X':  checkExit(); break;  // never called here, we actually catch it sooner below
            default:
                checkExit();  // shut everything down, close files
                char err1[17] = "BAD COMMAND: 'X'";
                err1[14] = currInstruction;
                updateLCD(err1, "ENDING PROFILE..");
                delay(3000);
                currentState = kGetProfile;  // go back to a known state
                break;
        }  // switch

    } // if

    // Safety
    if ( (int)currTemp > 275 ) {
        updateLCD(" Error: Invalid ", "  Temperature   ");
        checkExit(); // close files, turn off oven
        while (1);   // and lock up
    }

}

// Main loop
void loop(void)
{
    currTime = millis();

    // always get a temperature reading every second...
    if (currTime > timers[kTEMPTIMER]) {
        timers[kTEMPTIMER] = currTime + 1000;
        updateTemp();
    }

    // main state machine
    switch (currentState) {

        case kGetProfile:
            DEBUGLN("kGetProfile");

            // update the LCD to tell us about the last profile we used, OR
            // Profile 0 if we haven't used any yet.
            if (selectedprofile == kNOPROFILESELECTED) {
                showProfile(0);
            }
            else {
                showProfile(selectedprofile);
            }

            currInstruction = 'I';    // no instructions yet, _I_nitial state

            DEBUGLN("getting profile");

            // let the user pick a profile to run
            // only profiles that exist can be picked!
            selectedprofile = kNOPROFILESELECTED;
            while (selectedprofile == kNOPROFILESELECTED) {
                selectedprofile = getProfile();
            }

            DEBUGLN("opening file");

            sdRWfile('o', selectedprofile, 'i'); // Open profile spec file for input, e.g. "1.txt"
            sdRWfile('o', selectedprofile, 'o'); // Open profile log file for output, e.g. "1-out.txt"
            logFileStat = kLOGFILEOPEN;  // log file is OPEN

            while (checkcapbutton(kSQUAREBUTTON)); //wait until select button is released

            flashRELAY2 = 0;  // when flashing starts for the D command, always start with the LED ON state...
            timers[kINSTRUCTIONTIMER] = timers[kTEMPTIMER];   // main timer and temp timer are synchronized now
            log_time_in_seconds = 0;           // init the time that goes into the log
            instructionExpire = 0;             // init the expiration timer

            DEBUGLN("going to run state");
            currentState = kRunningProfile;
            break;

        case kRunningProfile:
            // states T, H, W, D, X
            running(currTime, true);  // normal RUN state, and update the display

            if (checkcapbutton(kSQUAREBUTTON)) { // if [] is pressed
                timers[kCANCELTIMER] = currTime + kCANCELTIMEOUT;
                updateLCD("\xDB to CANCEL", "^ to GO BACK");
                currentState = kFirstCancelDown;  // go to cancelling state
                DEBUGLN("going to kFirstCancelDown");
            }
            // else stay in kRunningProfile state
            else if (currInstruction == 'X') { // special processing for end of profile is here
                DEBUGLN("we are done");
                // we're done.
                checkExit();  // close the files, turn off the oven
                updateLCD("** PROFILE IS **", "** COMPLETE **");
                delay(3000);
                currentState = kGetProfile;
            }
            break;

        case kFirstCancelDown:
            running(currTime, false);  // profile is still running, until we complete the CANCEL operation

            if (!checkcapbutton(kSQUAREBUTTON)) { // wait until select button is released
                DEBUGLN("cancel button went up!");
                currentState = kCancelling;
            }
            // else stay in FirstCancelDown state // TODO: return to run state after N seconds of no keys?
            break;

        case kCancelling:
            // Square DOWN and UP received
            running(currTime, false);  // profile is still running, until we complete the CANCEL operation

            // Gotta press it a second time to confirm

            // if [] is pressed again, OR if we time
            if ( checkcapbutton(kSQUAREBUTTON) ) {
                DEBUGLN("second cancel pressed...");

                checkExit();  // turn off oven, close files

                updateLCD("PROFILE", "CANCELLED");
                delay(3000);
                currentState = kSecondCancelDown;
            }
            else if (checkcapbutton(kUPBUTTON) ||
                     checkcapbutton(kDOWNBUTTON) ||
                     currTime > timers[kCANCELTIMER] ) {
                DEBUGLN("some other button pressed, or timeout -- going back to RUN state");
                currentState = kRunningProfile;
            }
            // else stay in this state // TODO: return to run state after N seconds of no keys?
            break;

        case kSecondCancelDown:
            DEBUGLN("kSecondCancelDown");
            if (!checkcapbutton(kSQUAREBUTTON)) { // wait until select button is released
                DEBUGLN("cancel button released again");
                currentState = kGetProfile;  // and then go back to get a new profile
            }
            // else stay in this state
            break;

        default:
            DEBUGLN("some other weird state -- should never occur");
            updateLCD("** ERROR 1 **", "");
            delay(3000);
            currentState = kGetProfile;  // go back to a known state
            break;
    }


} // loop

