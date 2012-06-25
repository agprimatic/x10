/************************************************************************/
/* X10 PLC, RF, IR library test sketch with REST & JSON support, v1.1.  */
/*                                                                      */
/* This library is free software: you can redistribute it and/or modify */
/* it under the terms of the GNU General Public License as published by */
/* the Free Software Foundation, either version 3 of the License, or    */
/* (at your option) any later version.                                  */
/*                                                                      */
/* This library is distributed in the hope that it will be useful, but  */
/* WITHOUT ANY WARRANTY; without even the implied warranty of           */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU     */
/* General Public License for more details.                             */
/*                                                                      */
/* You should have received a copy of the GNU General Public License    */
/* along with this library. If not, see <http://www.gnu.org/licenses/>. */
/*                                                                      */
/* Written by Thomas Mittet thomas@mittet.nu November 2010.             */
/************************************************************************/

#include <X10ex.h>
#include <X10rf.h>
#include <X10ir.h>
#include <SPI.h>
#include <Ethernet.h>

#define DEBUG 0

#define POWER_LINE_MSG "PL:"
#define POWER_LINE_MSG_TIME 1400
#define RADIO_FREQ_MSG "RF:"
#define INFRARED_MSG "IR:"
#define SERIAL_DATA_MSG "SD:"
#define SERIAL_DATA_THRESHOLD 1000
#define ETHERNET_REST_MSG "ER:"
#define MODULE_STATE_MSG "MS:"
#define MSG_BUFFER_ERROR "_ExBuffer"
#define MSG_DATA_ERROR "_ExSyntax"
#define MSG_RECEIVE_TIMEOUT "_ExTimOut"
#define MSG_AUTH_ERROR "_ExNoAuth"
#define MSG_METHOD_ERROR "_ExMethod"

// Default username and password are "test" and "test". NOTE: With basic authentication user name and password is sent in clear text.
// To generate Base64 string first concatenate the user name and password using colon as a separator. Ex: testusername:testpassword
// To encode the username:password string use an online encoder like: "http://www.opinionatedgeek.com/dotnet/tools/base64encode/".
// NOTE: If you would like to disable Basic Authentication, just set the HTTP_AUTH_BASE64 define to an empty string "".
#define HTTP_AUTH_BASE64 "dGVzdDp0ZXN0"

// The buffer is used to hold data when reading HTTP request lines. If you are using a very long username and password, you might
// need to increase the buffer size. Using long post strings, for example when executing scenarios with multiple extended commands,
// might also cause the buffer to run out of space. With a buffer size of 64, you can send a scenario with 20 individual standard
// commands or 7 individual extended commands. A standard command is 3 bytes long and an extended command is 9 bytes long.
#define HTTP_BUFFER_MAX 64

// I recommend disabling the "Expect: 100-continue" on your HTTP client. When posting data with this feature enabled the HTTP client
// will send an "Expect: 100-continue" request in the header and then wait for the server to respond with a "100 Continue" before
// sending the body. This makes HTTP posts unnecessarily slow, especially when using slow cell phone connections. The default response
// timeout is 5 seconds. To disable "Expect: 100-continue" support on the Arduino: just set the timeout defined below to 0.
#define HTTP_CONTINUE_TIMEOUT 5000

// Ethernet receive HTTP parser constants
#define HTTP_STATE_PARSE_METHOD   0
#define HTTP_STATE_AUTHENTICATE   1
#define HTTP_STATE_WAIT_CONTINUE  2
#define HTTP_STATE_CONTINUE       3
#define HTTP_STATE_BODY_DONE      4
#define HTTP_METHOD_UNKNOWN  0
#define HTTP_METHOD_GET      1
#define HTTP_METHOD_POST     2
#define HTTP_METHOD_DELETE   3

// Fields used for serial and byte message reception
unsigned long sdReceived;
char bmHouse;
byte bmUnit;
byte bmCommand;
byte bmExtCommand;

// Choose a MAC-address and IP-address for your controller below.
// The IP address you should choose depends on your network setup:
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(10, 0, 0, 3);

// X10 Power Line Communication Library
X10ex x10ex = X10ex(
  2, // Zero Cross Interrupt Number (2 = "Custom" Pin Change Interrupt)
  4, // Zero Cross Interrupt Pin (Pin 4-7 can be used with interrupt 2)
  5, // Power Line Transmit Pin 
  6, // Power Line Receive Pin
  true, // Enable this to see echo of what is transmitted on the power line
  powerLineEvent, // Event triggered when power line message is received
  1, // Number of phases (1 = No Phase Repeat/Coupling)
  50 // The power line AC frequency (e.g. 50Hz in Europe, 60Hz in North America)
);
// X10 RF Receiver Library
X10rf x10rf = X10rf(
  0, // Receive Interrupt Number (0 = Standard Arduino External Interrupt)
  2, // Receive Interrupt Pin (Pin 2 must be used with interrupt 0)
  radioFreqEvent // Event triggered when RF message is received
);
// X10 Infrared Receiver Library
X10ir x10ir = X10ir(
  1, // Receive Interrupt Number (1 = Standard Arduino External Interrupt)
  3, // Receive Interrupt Pin (Pin 3 must be used with interrupt 1)
  'A', // Default House Code
  infraredEvent // Event triggered when IR message is received
);
// Ethernet server library
EthernetServer server = EthernetServer(
  80 // Start listening on port 80 (http)
);

void setup()
{
  // Remember to set baud rate in Serial Monitor or lower this to 9600 (default value)
  Serial.begin(115200);
  // Start the PLC, RF and IR libraries
  x10ex.begin();
  x10rf.begin();
  x10ir.begin();
  // Start the Ethernet Server library
  Ethernet.begin(mac, ip);
  server.begin();
  // X10 is printed in Serial Monitor at startup if you have connected your Arduino correctly
  Serial.println("X10");
}

void loop()
{
  if(!Serial.available()) ethernetReceive();
}

// Process messages received from X10 modules over the power line
void powerLineEvent(char house, byte unit, byte command, byte extData, byte extCommand, byte remainingBits)
{
  printX10Message(POWER_LINE_MSG, house, unit, command, extData, extCommand, remainingBits);
}

// Process commands received from X10 compatible RF remote
void radioFreqEvent(char house, byte unit, byte command, bool isRepeat)
{
  if(!isRepeat) printX10Message(RADIO_FREQ_MSG, house, unit, command, 0, 0, 0);
  // Check if command is handled by scenario; if not continue
  if(!handleUnitScenario(house, unit, command, isRepeat, false))
  {
    // Make sure that two or more repetitions are used for bright and dim,
    // to avoid that commands are beeing buffered seperately when repeated
    if(command == CMD_BRIGHT || command == CMD_DIM)
    {
      x10ex.sendCmd(house, unit, command, 2);
    }
    // Other commands map directly: just forward to PL interface
    else if(!isRepeat)
    {
      x10ex.sendCmd(house, unit, command, 1);
    }
  }
}

// Process commands received from X10 compatible IR remote
void infraredEvent(char house, byte unit, byte command, bool isRepeat)
{
  if(!isRepeat) printX10Message(INFRARED_MSG, house, unit, command, 0, 0, 0);
  // Check if command is handled by scenario; if not continue
  if(!handleUnitScenario(house, unit, command, isRepeat, false))
  {
    // Make sure that two or more repetitions are used for bright and dim,
    // to avoid that commands are beeing buffered seperately when repeated
    if(command == CMD_BRIGHT || command == CMD_DIM)
    {
      x10ex.sendCmd(house, unit, command, 2);
    }
    // Only repeat bright and dim commands
    else if(!isRepeat)
    {
      // Handle Address Command (House + Unit)
      if(command == CMD_ADDRESS)
      {
        x10ex.sendAddress(house, unit, 1);
      }
      // Other commands map directly: just forward to PL interface
      else
      {
        x10ex.sendCmd(house, unit, command, 1);
      }
    }
  }
}

// Process serial data messages received from computer over USB, Bluetooth, e.g.
//
// Serial messages are 3 or 9 bytes long. Use hex 0-F to address units and send commands.
// Bytes must be sent within one second (defined threshold) from the first to the last
// Below are some examples:
//
// Standard Messages examples:
// A12 (House=A, Unit=2, Command=On)
// AB3 (House=A, Unit=12, Command=Off)
// A_5 (House=A, Unit=N/A, Command=Bright)
// |||
// ||+-- Command 0-F or _  Example: 2 = On, 7 = ExtendedCode and _ = No Command
// |+--- Unit 0-F or _     Example: 0 = Unit 1, F = Unit 16 and _ = No unit
// +---- House code A-P    Example: A = House A and P = House P :)
//
// Extended Message examples:
// A37x31x21 (House=A, Unit=4, Command=ExtendedCode, Extended Command=PreSetDim, Extended Data=33)
// B87x01x0D (House=B, Unit=9, Command=ExtendedCode, Extended Command=ShutterOpen, Extended Data=13)
//     |/ |/
//     |  +-- Extended Data byte in hex     Example: 01 = 1%, 1F = 50% and 3E = 100% brightness (range is decimal 0-62)
//     +----- Extended Command byte in hex  Example: 31 = PreSetDim, for more examples check the X10 ExtendedCode spec.
//
// Scenario Execute examples:
// S03 (Execute scenario 3)
// S14 (Execute scenario 20)
// ||/
// |+--- Scenario byte in hex (Hex: 00-FF, Dec: 0-255)
// +---- Scenario Execute Character
//
// Request Module State examples:
// R** (Request buffered state of all modules)
// RG* (Request buffered state of modules using house code G)
// RA2 (Request buffered state of module A3)
// |||
// ||+-- Unit 0-F or *        Example: 0 = Unit 1, A = Unit 10 and * = All units
// |+--- House code A-P or *  Example: A = House A, P = House P and * = All house codes
// +---- Request Module State Character
//
// Wipe Module State examples:
// RW* (Wipe state data for all modules)
// RWB (Wipe state data for all modules using house code B)
// |||
// ||+-- House code A-P or *  Example: A = House A, P = House P and * = All house codes
// |+--- Wipe Module State Character
// +---- Request Module State Character
//
void serialEvent()
{
  // Read 3 bytes from serial buffer
  if(Serial.available() >= 3)
  {
    byte byte1 = toupper(Serial.read());
    byte byte2 = toupper(Serial.read());
    byte byte3 = toupper(Serial.read());
    if(process3BMessage(SERIAL_DATA_MSG, byte1, byte2, byte3))
    {
      // Return error message if message sent to X10ex was not buffered successfully
      Serial.print(SERIAL_DATA_MSG);
      Serial.println(MSG_BUFFER_ERROR);
    }
    sdReceived = 0;
  }
  // If partial message was received
  if(Serial.available() && Serial.available() < 3)
  {
    // Store received time
    if(!sdReceived)
    {
      sdReceived = millis();
    }
    // Clear message if all bytes were not received within threshold
    else if(sdReceived > millis() || millis() - sdReceived > SERIAL_DATA_THRESHOLD)
    {
      bmHouse = 0;
      bmExtCommand = 0;
      sdReceived = 0;
      Serial.print(SERIAL_DATA_MSG);
      Serial.println(MSG_RECEIVE_TIMEOUT);
      // Clear serial input buffer
      while(Serial.read() != -1);
    }
  }
}

// Process request received from Arduino Ethernet Shield
void ethernetReceive()
{
  EthernetClient client = server.available();
  if(client)
  {
    while(client.connected())
    {
      byte readState = HTTP_STATE_PARSE_METHOD;
      byte parsingEncoded = 0;
      byte encodedChar;
      bool parsingText = false;
      byte bufferMax = HTTP_BUFFER_MAX;
      char buffer[HTTP_BUFFER_MAX + 1];
      byte lastLineLen = HTTP_BUFFER_MAX;
      byte method = HTTP_METHOD_UNKNOWN;
      char house = '*';
      byte unit = 0;
      bool x10exBufferError = 0;
      while(client.available())
      {
        char c = client.read();
        // Handle encoded characters
        if(c == '%')
        {
          parsingEncoded = 2;
        }
        else if(parsingEncoded)
        {
          parsingEncoded--;
          if(parsingEncoded) encodedChar = charHexToDecimal(c) * 16;
          else c = charHexToDecimal(c) + encodedChar;
        }
        // Handle "normal" characters
        if(!parsingEncoded)
        {
          // Enable quoted text parser (allows lower case and space)
          if(c == '"')
          {
            parsingText = !parsingText;
          }
          // Space is only stored when parsing HTTP method or quoted text
          else if(isgraph(c) || ((readState == HTTP_STATE_PARSE_METHOD || parsingText) && c == ' '))
          {
            // Space is normally replaced with '+' in encoded strings
            if(parsingText && c == '+') c = ' ';
            // When parsing quoted text or Base64 string, characters are not converted to upper case
            buffer[HTTP_BUFFER_MAX - bufferMax] =
              parsingText || readState == HTTP_STATE_AUTHENTICATE ?
              c : toupper(c);
            buffer[HTTP_BUFFER_MAX - --bufferMax] = '\0';
          }
        }
        // We have reached end of line, buffer or transmission
        if(c == '\n' || !bufferMax || !client.available())
        {
          byte lineLen = strlen(buffer);
          // Parse request method and path
          if(readState == HTTP_STATE_PARSE_METHOD)
          {
            // If method is GET
            if(!strncmp(buffer, "GET", 3))
            {
              method = HTTP_METHOD_GET;
            }
            // If method is POST
            else if(!strncmp(buffer, "POST", 4))
            {
              method = HTTP_METHOD_POST;
            }
            // If method is DELETE
            else if(!strncmp(buffer, "DELETE", 6))
            {
              method = HTTP_METHOD_DELETE;
            }
            // Parse path (house and unit)
            byte ix = stringIndexOf(buffer, '/', 0, 0, 0);
            if(buffer[ix] == '/')
            {
              char data = buffer[ix + 1];
              if(data >= 'A' && data <= 'P')
              {
                house = data;
                if(buffer[ix + 2] == '/')
                {
                  unit = stringToDecimal(buffer, ix + 3, ix + 5);
                }
              }
            }
            readState = HTTP_STATE_AUTHENTICATE;
          }
          // Validate user credentials (stored in Base64 string)
          else if(readState == HTTP_STATE_AUTHENTICATE)
          {
            if(!lineLen) break;
            byte base64Len = strlen(HTTP_AUTH_BASE64);
            // Base64 string should be found at position 19 excluding whitespace
            if(!base64Len || (base64Len == lineLen - 19 && !strncmp(buffer + 19, HTTP_AUTH_BASE64, base64Len)))
            {
              readState = HTTP_STATE_CONTINUE;
            }
          }
#if HTTP_CONTINUE_TIMEOUT > 0
          // Look for "Expect: 100-continue"
          else if(readState == HTTP_STATE_CONTINUE && method == HTTP_METHOD_POST && !strncmp(buffer, "EXPECT:100-CONTINUE", 19))
          {
            client.println("HTTP/1.1 100 Continue\n");
            readState = HTTP_STATE_WAIT_CONTINUE;
          }
          // Wait for client to receive "100 Continue" and start sending body
          else if(readState == HTTP_STATE_WAIT_CONTINUE && !client.available() && lastLineLen)
          {
            int i;
            for(int i = HTTP_CONTINUE_TIMEOUT; i > 0 && !client.available(); i--)
            {
              delay(1);
            }
            if(!i)
            {
              Serial.print(ETHERNET_REST_MSG);
              Serial.println(MSG_RECEIVE_TIMEOUT);
            }
            readState = HTTP_STATE_CONTINUE;
          }
#endif
          // Parse body and execute commands, after blank line seperating body from head has been received
          if(readState >= HTTP_STATE_WAIT_CONTINUE && method == HTTP_METHOD_POST && !lastLineLen)
          {
            char cmdHouse = house;
            byte cmdUnit = unit;
            byte bufferLen = stringIndexOf(buffer, '\0', 0, HTTP_BUFFER_MAX, HTTP_BUFFER_MAX);
            byte endIx = 0;
            while(endIx < bufferLen)
            {
              byte startIx = endIx > 0 ? endIx + 1 : 0;
              // Update command end index
              endIx = stringIndexOf(buffer, '&', startIx, bufferLen, bufferLen);
              // Get index of equal sign
              byte eq = stringIndexOf(buffer, '=', startIx, endIx, 0);
              if(eq > startIx)
              {
                // If user specified house code, use it in stead of the one parsed from path
                if(!strncmp(buffer + startIx, "HOUSE", eq - startIx))
                {
                  cmdHouse = buffer[eq + 1];
                }
                // If user specified unit code, use it in stead of the one parsed from path
                else if(!strncmp(buffer + startIx, "UNIT", eq - startIx))
                {
                  cmdUnit = stringToDecimal(buffer, eq + 1, endIx);
                }
                // Parse set module type command (0 = Unknown, 1 = Appliance, 2 = Dimmer, 3 = Sensor)
                else if(!strncmp(buffer + startIx, "TYPE", eq - startIx))
                {
                  x10ex.setModuleType(cmdHouse, cmdUnit, stringToDecimal(buffer, eq + 1, endIx));
                }
                // Parse set module name command (max. 16 characters)
                else if(!strncmp(buffer + startIx, "NAME", eq - startIx))
                {
                  x10ex.setModuleName(cmdHouse, cmdUnit, buffer + eq + 1, endIx - eq - 1);
                }
                // Parse on command (true, false, 1 and 0 are valid input)
                else if(!strncmp(buffer + startIx, "ON", eq - startIx))
                {
                  byte cmd;
                  cmd =
                    !strncmp(buffer + eq + 1, "TRUE", endIx - eq - 1 > 4 ? endIx - eq - 1 : 4) ||
                    !strncmp(buffer + eq + 1, "1", endIx - eq - 1) ?
                    CMD_ON : CMD_OFF;
                  // Check if command is handled by scenario; if not continue
                  if(!handleUnitScenario(cmdHouse, cmdUnit, cmd, false, true))
                  {
                    x10exBufferError = x10ex.sendCmd(cmdHouse, cmdUnit, cmd, 2);
                  }
                  if(!x10exBufferError)
                  {
                    printX10Message(ETHERNET_REST_MSG, cmdHouse, cmdUnit, cmd, 0, 0, 0);
                    delay(POWER_LINE_MSG_TIME);
                  }
                  break;
                }
                // Parse brightness command (0-100 percent)
                else if(!strncmp(buffer + startIx, "BRIGHTNESS", eq - startIx))
                {
                  byte brightness = x10ex.percentToX10Brightness(stringToDecimal(buffer, eq + 1, endIx));
                  x10exBufferError = x10ex.sendExt(cmdHouse, cmdUnit, CMD_EXTENDED_CODE, brightness, EXC_PRE_SET_DIM, 2);
                  if(!x10exBufferError)
                  {
                    printX10Message(ETHERNET_REST_MSG, cmdHouse, cmdUnit, CMD_EXTENDED_CODE, brightness, EXC_PRE_SET_DIM, 0);
                    delay(POWER_LINE_MSG_TIME);
                  }
                  break;
                }
                // Parse 3 byte command (Up to 16, 3 or 9 byte commands, are supported)
                else if(!strncmp(buffer + startIx, "CMD", eq - startIx))
                {
                  while(endIx - eq - 1 >= 3 && !x10exBufferError)
                  {
                    x10exBufferError = process3BMessage(ETHERNET_REST_MSG, buffer[eq + 1], buffer[eq + 2], buffer[eq + 3]);
                    eq += 3;
                  }
                  break;
                }
              }
            }
            readState = HTTP_STATE_BODY_DONE;
          }
          // Execute delete module state and info
          else if(readState == HTTP_STATE_CONTINUE && method == HTTP_METHOD_DELETE)
          {
            x10ex.wipeModuleState(house, unit);
            x10ex.wipeModuleInfo(house, unit);
          }
          parsingText = false;
          bufferMax = HTTP_BUFFER_MAX;
          buffer[0] = '\0';
          lastLineLen = lineLen;
        }
        // Check if we are done receiving
        if((readState == HTTP_STATE_CONTINUE && method != HTTP_METHOD_POST) || readState == HTTP_STATE_BODY_DONE) break;
      }
      client.print("HTTP/1.1");
      // State AUTH_START means user has not provided valid credentials yet
      if(readState <= HTTP_STATE_AUTHENTICATE)
      {
        client.println(" 401 Authorization Required\nWWW-Authenticate: Basic realm=\"Secure Area\"\nContent-Type: text/html\n");
        client.print("<html><body>401 Unauthorized</body></html>");
        Serial.print(ETHERNET_REST_MSG);
        Serial.println(MSG_AUTH_ERROR);
      }
      // User is trying to execute unsupported HTTP request method
      else if(method == HTTP_METHOD_UNKNOWN)
      {
        client.println(" 501 Not Implemented\nContent-Type: application/json\n");
        Serial.print(ETHERNET_REST_MSG);
        Serial.println(MSG_METHOD_ERROR);
      }
      // Response is always sent after successful GET, POST or DELETE request
      else
      {
        // Return JSON response
        client.println(" 200 OK\nContent-Type: application/json\n");
        if(house != '*' && unit > 0 && unit <= 16)
        {
          erPrintModuleState(client, house, unit, true, true);
        }
        else
        {
          client.println("{\n\"module\":\n[");
          bool isFirst = true;
          // All units using specified house code
          if(house != '*')
          {
            for(byte i = 1; i <= 16; i++)
            {
              if(erPrintModuleState(client, house, i, isFirst, false)) isFirst = false;
            }
          }
          // All units
          else
          {
            for(short i = 0; i < 256; i++)
            {
              if(erPrintModuleState(client, (i >> 4) + 0x41, (i & 0xF) + 1, isFirst, false)) isFirst = false;
            }
          }
          client.print("\n]\n}");
        }
      }
      delay(1);
      client.stop();
    }
  }
}

// Processes and executes 3 byte serial and ethernet messages.
bool process3BMessage(const char type[], byte byte1, byte byte2, byte byte3)
{
  bool x10exBufferError = 0;
  // Convert byte2 from hex to decimal unless command is request module state
  if(byte1 != 'R') byte2 = charHexToDecimal(byte2);
  // Convert byte3 from hex to decimal unless command is wipe module state
  if(byte1 != 'R' || byte2 != 'W') byte3 = charHexToDecimal(byte3);
  // Check if standard message was received (byte1 = House, byte2 = Unit, byte3 = Command)
  if(byte1 >= 'A' && byte1 <= 'P' && (byte2 <= 0xF || byte2 == '_') && (byte3 <= 0xF || byte3 == '_') && (byte2 != '_' || byte3 != '_'))
  {
    // Store first 3 bytes of message to make it possible to process 9 byte serial messages
    bmHouse = byte1;
    bmUnit = byte2 == '_' ? 0 : byte2 + 1;
    bmCommand = byte3 == '_' ? CMD_STATUS_REQUEST : byte3;
    bmExtCommand = 0;
    // Send standard message if command type is not extended
    if(bmCommand != CMD_EXTENDED_CODE && bmCommand != CMD_EXTENDED_DATA)
    {
      printX10Message(type, bmHouse, bmUnit, byte3, 0, 0, 8 * Serial.available());
      // Check if command is handled by scenario; if not continue
      if(!handleUnitScenario(bmHouse, bmUnit, bmCommand, false, true))
      {
        x10exBufferError = x10ex.sendCmd(bmHouse, bmUnit, bmCommand, bmCommand == CMD_BRIGHT || bmCommand == CMD_DIM ? 2 : 1);
      }        
      bmHouse = 0;
    }
  }
  // Check if extended message was received (byte1 = Hex Seperator, byte2 = Hex 1, byte3 = Hex 2)
  else if(byte1 == 'X' && bmHouse)
  {
    byte data = byte2 * 16 + byte3;
    // No extended command set, assume that we are receiving command
    if(!bmExtCommand)
    {
      bmExtCommand = data;
    }
    // Extended command set, we must be receiving extended data
    else
    {
      printX10Message(type, bmHouse, bmUnit, bmCommand, data, bmExtCommand, 8 * Serial.available());
      x10exBufferError = x10ex.sendExt(bmHouse, bmUnit, bmCommand, data, bmExtCommand, 1);
      bmHouse = 0;
    }
  }
  // Check if scenario execute command was received (byte1 = Scenario Character, byte2 = Hex 1, byte3 = Hex 2)
  else if(byte1 == 'S')
  {
    byte scenario = byte2 * 16 + byte3;
    Serial.print(type);
    Serial.print("S");
    if(scenario <= 0xF) { Serial.print("0"); }
    Serial.println(scenario, HEX);
    handleSdScenario(scenario);
  }
  // Check if request module state command was received (byte1 = Request State Character, byte2 = House, byte3 = Unit)
  else if(byte1 == 'R' && ((byte2 >= 'A' && byte2 <= 'P') || byte2 == '*'))
  {
    Serial.print(type);
    Serial.print("R");
    Serial.print(byte2);
    // All modules
    if(byte2 == '*')
    {
      Serial.println('*');
      for(short i = 0; i < 256; i++)
      {
        sdPrintModuleState((i >> 4) + 0x41, i & 0xF);
      }
    }
    else
    {
      if(byte3 <= 0xF)
      {
        Serial.println(byte3, HEX);
        sdPrintModuleState(byte2, byte3);
      }
      // All units using specified house code
      else
      {
        Serial.println('*');
        for(byte i = 0; i < 16; i++)
        {
          sdPrintModuleState(byte2, i);
        }
      }
    }
  }
  // Check if request wipe module state command was received (byte1 = Request State Character, byte2 = Wipe Character, byte3 = House)
  else if(byte1 == 'R' && byte2 == 'W' && ((byte3 >= 'A' && byte3 <= 'P') || byte3 == '*'))
  {
    Serial.print(type);
    Serial.print("RW");
    Serial.println((char)byte3);
    x10ex.wipeModuleState(byte3);
    Serial.print(MODULE_STATE_MSG);
    Serial.print(byte3 >= 'A' && byte3 <= 'P' ? (char)byte3 : '*');
    Serial.println("__");
  }
  // Unknown command/data
  else
  {
    Serial.print(type);
    Serial.println(MSG_DATA_ERROR);
  }
  return x10exBufferError;
}

void printX10Message(const char type[], char house, byte unit, byte command, byte extData, byte extCommand, int remainingBits)
{
  printX10TypeHouseUnit(type, house, unit, command);
  // Ignore non X10 commands like the CMD_ADDRESS command used by the IR library
  if(command <= 0xF)
  {
    Serial.print(command, HEX);
    if(extCommand || (extData && (command == CMD_STATUS_ON || command == CMD_STATUS_OFF)))
    {
      printX10ByteAsHex(extCommand);
      printX10ByteAsHex(extCommand == EXC_PRE_SET_DIM ? extData & B111111 : extData);
    }
  }
  else
  {
    Serial.print("_");
  }
  Serial.println();
#if DEBUG
  printDebugX10Message(type, house, unit, command, extData, extCommand, remainingBits);
#endif
}

void printX10TypeHouseUnit(const char type[], char house, byte unit, byte command)
{
  Serial.print(type);
  Serial.print(house);
  if(
    unit &&
    unit != DATA_UNKNOWN/* &&
    command != CMD_ALL_UNITS_OFF &&
    command != CMD_ALL_LIGHTS_ON &&
    command != CMD_ALL_LIGHTS_OFF &&
    command != CMD_HAIL_REQUEST*/)
  {
    Serial.print(unit - 1, HEX);
  }
  else
  {
    Serial.print("_");
  }
}

void sdPrintModuleState(char house, byte unit)
{
  unit++;
  X10state state = x10ex.getModuleState(house, unit);
  if(state.isSeen)
  {
    printX10Message(
      MODULE_STATE_MSG, house, unit,
      state.isKnown ? state.isOn ? CMD_STATUS_ON : CMD_STATUS_OFF : DATA_UNKNOWN,
      state.data,
      0, 0);
  }
}

bool erPrintModuleState(EthernetClient client, char house, byte unit, bool isFirst, bool printUnseenModules)
{
  X10state state = x10ex.getModuleState(house, unit);
  X10info info = x10ex.getModuleInfo(house, unit);
  if(state.isSeen || printUnseenModules)
  {
    if(!isFirst) client.println(",");
    client.print("{\n\"house\": \"");
    client.print(house);
    client.println("\",");
    client.print("\"unit\": ");
    client.print(unit, DEC);
    client.print(",\n\"url\": \"/");
    client.print(house);
    client.print("/");
    client.print(unit, DEC);
    client.print("/\"");
    if(info.type)
    {
      client.print(",\n\"type\": ");
      client.print(info.type, DEC);
    }
    if(strlen(info.name))
    {
      client.print(",\n\"name\": \"");
      client.print(info.name);
      client.print("\"");
    }
    if(state.isKnown)
    {
      client.print(",\n\"on\": ");
      client.print(state.isOn ? "true" : "false");
      if(state.data)
      {
        client.print(",\n\"brightness\": ");
        client.print(x10ex.x10BrightnessToPercent(state.data), DEC);
      }
    }
    client.print("\n}");
    return true;
  }
  return false;
}

void printX10ByteAsHex(byte data)
{
  Serial.print("x");
  if(data <= 0xF) { Serial.print("0"); }
  Serial.print(data, HEX);
}

byte charHexToDecimal(byte input)
{
  // 0123456789  =>  0-15
  if(input >= 0x30 && input <= 0x39) input -= 0x30;
  // ABCDEF  =>  10-15
  else if(input >= 0x41 && input <= 0x46) input -= 0x37;
  // Return converted byte
  return input;
}

byte stringToDecimal(const char input[], byte startPos, byte endPos)
{
  byte decimal = 0;
  byte multiplier = 1;
  for(byte i = endPos + 1; i > startPos; i--)
  {
    if(input[i - 1] >= 0x30 && input[i - 1] <= 0x39)
    {
      decimal += (input[i - 1] - 0x30) * multiplier;
      multiplier *= 10;
    }
    else if(multiplier > 1)
    {
      break;
    }
  }
  return decimal;
}

short stringIndexOf(const char string[], char find, byte startPos, byte endPos, short notFoundValue)
{
  char *ixStr = strchr(string + startPos, find);
  return
    ixStr - string < 0 || (endPos > 0 && ixStr - string > endPos) ?
    notFoundValue : ixStr - string;
}

// Handles scenario execute commands received as serial data message
void handleSdScenario(byte scenario)
{
  switch(scenario)
  {
    //////////////////////////////
    // Sample Code
    // Replace with your own setup
    //////////////////////////////
    case 0x01: sendAllLightsOn(); break;
    case 0x02: sendHallAndKitchenOn(); break;
    case 0x03: sendLivingRoomOn(); break;
    case 0x04: sendLivingRoomTvScenario(); break;
    case 0x05: sendLivingRoomMovieScenario(); break;
    case 0x11: sendAllLightsOff(); break;
    case 0x12: sendHallAndKitchenOff(); break;
    case 0x13: sendLivingRoomOff(); break;
  }
}

// Handles scenarios triggered when receiving unit on/off commands
// Use this method to trigger scenarios from simple RF/IR remotes
bool handleUnitScenario(char house, byte unit, byte command, bool isRepeat, bool failOnBufferError)
{
  //////////////////////////////
  // Sample Code
  // Replace with your own setup
  //////////////////////////////

  bool bufferError = 0;

  // Ignore all house codes except A
  if(house != 'A') return 0;

  // Unit 4 is an old X10 lamp module that doesn't remember state on its own. Use the
  // following method to revert to buffered state when these modules are turned on
  if(unit == 4)
  {
    bufferError = handleOldLampModuleState(house, unit, command, isRepeat);
  }  
  // Unit 5 is a placeholder unit code used by RF and IR remotes that triggers HallAndKitchen scenario
  else if(unit == 5)
  {
    if(!isRepeat)
    {
      if(command == CMD_ON) bufferError = sendHallAndKitchenOn();
      else if(command == CMD_OFF) bufferError = sendHallAndKitchenOff();
    }
  }
  // Unit 10 is a placeholder unit code used by RF and IR remotes that triggers AllLights scenario
  else if(unit == 10)
  {
    if(!isRepeat)
    {
      if(command == CMD_ON) bufferError = sendAllLightsOn();
      else if(command == CMD_OFF) bufferError = sendAllLightsOff();
    }
  }
  // Unit 11 is a placeholder unit code used by RF and IR remotes that triggers LivingRoom scenario
  else if(unit == 11)
  {
    if(!isRepeat)
    {
      if(command == CMD_ON) bufferError = sendLivingRoomOn();
      else if(command == CMD_OFF) bufferError = sendLivingRoomOff();
    }
  }
  // Unit 12 is a placeholder unit code used by RF and IR remotes that triggers LivingRoomTv scenario
  else if(unit == 12)
  {
    if(!isRepeat)
    {
      if(command == CMD_ON) bufferError = sendLivingRoomTvScenario();
      else if(command == CMD_OFF) bufferError = sendLivingRoomOn();
    }
  }
  // Unit 13 is a placeholder unit code used by RF and IR remotes that triggers LivingRoomMovie scenario
  else if(unit == 13)
  {
    if(!isRepeat)
    {
      if(command == CMD_ON) bufferError = sendLivingRoomMovieScenario();
      else if(command == CMD_OFF) bufferError = sendLivingRoomOn();
    }
  }
  else
  {
    return 0;
  }
  return !failOnBufferError || !bufferError;
}

// Handles old X10 lamp modules that don't remember state on their own
bool handleOldLampModuleState(char house, byte unit, byte command, bool isRepeat)
{
  if(!isRepeat)
  {
    bool bufferError = 0;
    if(command == CMD_ON)
    {
      // Get state from buffer, and set to last brightness when turning on
      X10state state = x10ex.getModuleState(house, unit);
      if(state.isKnown && !state.isOn && state.data > 0)
      {
        bufferError = x10ex.sendExt(house, unit, CMD_EXTENDED_CODE, state.data, EXC_PRE_SET_DIM, 1);
      }
    }
    return bufferError || x10ex.sendCmd(house, unit, command, 1);
  }
  return 0;
}

//////////////////////////////
// Scenarios
// Replace with your own setup
//////////////////////////////

bool sendAllLightsOn()
{
  return
    // Bedroom
    x10ex.sendExtDim('A', 7, 80, EXC_DIM_TIME_4, 1) ||
    // Dining Table
    x10ex.sendExtDim('A', 2, 70, EXC_DIM_TIME_4, 1) ||
    // Hall
    x10ex.sendExtDim('A', 8, 75, EXC_DIM_TIME_4, 1) ||
    // Couch
    x10ex.sendExtDim('A', 3, 90, EXC_DIM_TIME_4, 1) ||
    // Kitchen
    x10ex.sendExtDim('A', 9, 100, EXC_DIM_TIME_4, 1) ||
    // TV Backlight
    x10ex.sendExtDim('A', 4, 40, EXC_DIM_TIME_4, 1);
}

bool sendAllLightsOff()
{
  return
    x10ex.sendCmd('A', 7, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 2, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 8, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 3, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 9, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 4, CMD_OFF, 1);
}

bool sendHallAndKitchenOn()
{
  return
    x10ex.sendExtDim('A', 8, 75, EXC_DIM_TIME_4, 1) ||
    x10ex.sendExtDim('A', 9, 100, EXC_DIM_TIME_4, 1);
}

bool sendHallAndKitchenOff()
{
  return
    x10ex.sendCmd('A', 8, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 9, CMD_OFF, 1);
}

bool sendLivingRoomOn()
{
  return
    x10ex.sendExtDim('A', 2, 70, EXC_DIM_TIME_4, 1) ||
    x10ex.sendExtDim('A', 3, 90, EXC_DIM_TIME_4, 1) ||
    x10ex.sendExtDim('A', 4, 40, EXC_DIM_TIME_4, 1);
}

bool sendLivingRoomOff()
{
  return
    x10ex.sendCmd('A', 2, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 3, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 4, CMD_OFF, 1);
}

bool sendLivingRoomTvScenario()
{
  return
    x10ex.sendExtDim('A', 2, 40, EXC_DIM_TIME_4, 1) ||
    x10ex.sendExtDim('A', 3, 30, EXC_DIM_TIME_4, 1) ||
    x10ex.sendExtDim('A', 4, 25, EXC_DIM_TIME_4, 1);
}

bool sendLivingRoomMovieScenario()
{
  return
    x10ex.sendCmd('A', 2, CMD_OFF, 1) ||
    x10ex.sendCmd('A', 3, CMD_OFF, 1) ||
    x10ex.sendExtDim('A', 4, 25, EXC_DIM_TIME_4, 1);
}

#if DEBUG

void printDebugX10Message(const char type[], char house, byte unit, byte command, byte extData, byte extCommand, int remainingBits)
{
  Serial.print("DEBUG=");
  printX10TypeHouseUnit(type, house, unit, command);
  switch(command)
  {
    // This is not a real X10 command, it's a special command used by the IR
    // library to signal that an address and a house code has been received
    case CMD_ADDRESS:
      break;
    case CMD_ALL_UNITS_OFF:
      Serial.println("_AllUnitsOff");
      break;
    case CMD_ALL_LIGHTS_ON:
      Serial.println("_AllLightsOn");
      break;
    case CMD_ON:
      Serial.print("_On");
      printDebugX10Brightness("_Brightness", extData);
      break;
    case CMD_OFF:
      Serial.print("_Off");
      printDebugX10Brightness("_Brightness", extData);
      break;
    case CMD_DIM:
      Serial.println("_Dim");
      break;
    case CMD_BRIGHT:
      Serial.println("_Bright");
      break;
    case CMD_ALL_LIGHTS_OFF:
      Serial.println("_AllLightsOff");
      break;
    case CMD_EXTENDED_CODE:
      Serial.print("_ExtendedCode");
      break;
    case CMD_HAIL_REQUEST:
      Serial.println("_HailReq");
      break;
    case CMD_HAIL_ACKNOWLEDGE:
      Serial.println("_HailAck");
      break;
    // Enable X10_USE_PRE_SET_DIM in X10ex header file
    // to use X10 standard message PRE_SET_DIM commands
    case CMD_PRE_SET_DIM_0:
    case CMD_PRE_SET_DIM_1:
      printDebugX10Brightness("_PreSetDim", extData);
      break;
    case CMD_EXTENDED_DATA:
      Serial.print("_ExtendedData");
      break;
    case CMD_STATUS_ON:
      Serial.println("_StatusOn");
      break;
    case CMD_STATUS_OFF:
      Serial.println("_StatusOff");
      break;
    case CMD_STATUS_REQUEST:
      Serial.println("_StatusReq");
      break;
    case DATA_UNKNOWN:
      Serial.println("_Unknown");
      break;
    default:
      Serial.println();
  }
  if(extCommand)
  {
    switch(extCommand)
    {
      case EXC_PRE_SET_DIM:
        printDebugX10Brightness("_PreSetDim", extData);
        break;
      default:
        Serial.print("_");
        Serial.print(extCommand, HEX);
        Serial.print("_");
        Serial.println(extData, HEX);
    }
  }
  if(remainingBits)
  {
    printX10TypeHouseUnit(type, house, unit, command);
    Serial.print("_ErrorBitCount=");
    Serial.println(remainingBits, DEC);
  }
}

void printDebugX10Brightness(const char source[], byte extData)
{
  if(extData > 0)
  {
    Serial.print(source);
    Serial.print("_");
    Serial.println(x10ex.x10BrightnessToPercent(extData), DEC);
  }
  else
  {
    Serial.println();
  }
}

#endif
