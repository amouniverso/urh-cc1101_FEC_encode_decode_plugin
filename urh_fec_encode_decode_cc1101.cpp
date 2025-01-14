/**************************************************************************************************************
*  	Universal Radio Hacker (urh) plugin to encode and decode cc1101 messages, 
*	which uses the cc1101 FEC (forward error correction) feature
*
*	simply compile with: g++ urh_fec_encode_decode_cc1101.cpp -o urh_fec_encode_decode_cc1101
*
*	start the plugin in urh as external program with: 
*	urh_fec_encode_decode_cc1101 [e | d | f]
*
*	Parameter description: 
*	----------------------
*
*	Decode FEC messages:
*	d = output decoded payload incl. preamble and sync word. 
*
*	Encode FEC messages:
*	e = output encoded payload incl. preamble and sync word.
*
*
**************************************************************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**************************************************************************************************************
* FUNCTION PROTOTYPES
*/
unsigned short fecDecode(unsigned char *pDecData, unsigned char* pInData, unsigned short RemBytes);
static unsigned char hammWeight(unsigned char a);
static unsigned char min(unsigned char a, unsigned char b);
static unsigned short calcCRC(unsigned char crcData, unsigned short crcReg);

int fromBinary(char *s);
void printbincharpad(char c);
/**************************************************************************************************************
* GLOBAL VARIABLES
*/

/*
* FEC encode variable and tables
*/

unsigned char mode_encode = 0;

const unsigned int fecEncodeTable[] = {
	0, 3, 1, 2,
	3, 0, 2, 1,
	3, 0, 2, 1,
	0, 3, 1, 2
};

/*
* FEC decode variable and tables
*/

unsigned char rxBuffer[4];  // Buffer used to hold data read from the RXFIFO (4 bytes are read at a time)
unsigned char rxPacket[8 * 64]; // Data + CRC after being interleaved and decoded

unsigned char rx_data[8 * 64] = {0};

unsigned char output_format = 0;
unsigned char mode_decode = 0;
unsigned char byte_count = 0;
unsigned short bit_count = 0;


// Look-up source state index when:
// Destination state --\ /-- Each of two possible source states
const unsigned char aTrellisSourceStateLut[8][2] =
{
 {0, 4}, // State {0,4} -> State 0
 {0, 4}, // State {0,4} -> State 1
 {1, 5}, // State {1,5} -> State 2
 {1, 5}, // State {1,5} -> State 3
 {2, 6}, // State {2,6} -> State 4
 {2, 6}, // State {2,6} -> State 5
 {3, 7}, // State {3,7} -> State 6
 {3, 7}, // State {3,7} -> State 7
};

// Look-up expected output when:
// Destination state --\ /-- Each of two possible source states
const unsigned char aTrellisTransitionOutput[8][2] =
{
 {0, 3}, // State {0,4} -> State 0 produces {"00", "11"}
 {3, 0}, // State {0,4} -> State 1 produces {"11", "00"}
 {1, 2}, // State {1,5} -> State 2 produces {"01", "10"}
 {2, 1}, // State {1,5} -> State 3 produces {"10", "01"}
 {3, 0}, // State {2,6} -> State 4 produces {"11", "00"}
 {0, 3}, // State {2,6} -> State 5 produces {"00", "11"}
 {2, 1}, // State {3,7} -> State 6 produces {"10", "01"}
 {1, 2}, // State {3,7} -> State 7 produces {"01", "10"}
};

// Look-up input bit at encoder when:
// Destination state --
const unsigned char aTrellisTransitionInput[8] =
{
 0,
 1,
 0,
 1,
 0,
 1,
 0,
 1,
};

/**************************************************************************************************************
* @fn hammWeight
*
* @brief Calculates Hamming weight of byte (# bits set)
*
* @param a - Byte to find the Hamming weight for
*
* @return Hamming weight (# of bits set in a)
*/
static unsigned char hammWeight(unsigned char a)
{
 a = ((a & 0xAA) >> 1) + (a & 0x55);
 a = ((a & 0xCC) >> 2) + (a & 0x33);
 a = ((a & 0xF0) >> 4) + (a & 0x0F);
 return a;
}

/**************************************************************************************************************
* @fn min
*
* @brief Returns the minimum of two values
*
* @param a - Value 1
* b - Value 2
*
* @return Minimum of two values
* Value 1 (Value 1 < Value 2)
* Value 2 (Value 2 < Value 1)
*/
static unsigned char min(unsigned char a, unsigned char b)
{
 return (a <= b ? a : b);
}

/**************************************************************************************************************
* @fn calcCRC
*
* @brief Calculates a checksum over n data bytes
* Example of usage
*
* checksum = 0xFFFF;
* for (i = 0; i < n; i++)
* checksum = calcCRC(dataBytes[i], checksum);
*
* @param crcData - checksum (initially set to 0xFFFF)
* crcReg - data byte
*
*
* @return Checksum
*/
static unsigned short calcCRC(unsigned char crcData, unsigned short crcReg)
{
 unsigned char i;
 for (i = 0; i < 8; i++) {
 	if (((crcReg & 0x8000) >> 8) ^ (crcData & 0x80))
 		crcReg = (crcReg << 1) ^ 0x8005;
 	else
 		crcReg = (crcReg << 1);
 	crcData <<= 1;
 }
 return crcReg;
} 

/**************************************************************************************************************
* @fn fecEncode
*
* @brief interleaves and encodes a given input buffer
*
* @param pEncData - Pointer to where to put encoded data
* pInData - Pointer to received data
* lenData - count of data to encode and interleave
*
*
* @return number of bytes of encoded and interleaved payload
*/

/*
	data to conver is payload without pktlen byte, rssi and crc byte
	example payload data:
	TX payload non-encoded: aaaaaaaa 57435743 06030100010203da05
	TX payload FEC-encoded: aaaaaaaa 57435743 88c83c000c3330124cf03010b8dca35340347fe3
	input = 030100010203
	output= 88c83c000c3330124cf03010b8dca35340347fe3
	return = 20
*/
unsigned short fecEncode(unsigned char *pEncData, unsigned char* pInData, unsigned char length)
{
	
	unsigned int i, j, val, fecReg, fecOutput;
	unsigned long intOutput;
	unsigned int fec[520];
	unsigned int interleaved[520];
	unsigned int inputNum = 0, fecNum;
	unsigned int checksum;

	pInData[0] = length;

	inputNum = length + 1;

	// Generate CRC
	checksum = 0xFFFF; //Init value for CRC calculation
	
	for(i = 0; i <= pInData[0]; i++){
	 checksum = calcCRC(pInData[i], checksum);
	}
	pInData[inputNum++] = checksum >> 8;     // CRC1
	pInData[inputNum++] = checksum & 0x00FF; // CRC0


	// Append Trellis Terminator
	pInData[inputNum] = 0x0B;
	pInData[inputNum + 1] = 0x0B;
	
	fecNum = 2*((inputNum / 2) + 1);
	

	// FEC encode
	fecReg = 0;
	for (i = 0; i < fecNum; i++) {
	 	fecReg = (fecReg & 0x700) | (pInData[i] & 0xFF);
	 	fecOutput = 0;
		for (j = 0; j < 8; j++) {
			fecOutput = (fecOutput << 2) | fecEncodeTable[fecReg >> 7];
		 	fecReg = (fecReg << 1) & 0x7FF;
		}
	 	fec[i * 2] = fecOutput >> 8;
	 	fec[i * 2 + 1] = fecOutput & 0xFF;
	}


	// Perform interleaving
	for (i = 0; i < fecNum * 2; i += 4) {
		intOutput = 0;
	 	for (j = 0; j < 4*4; j++){
	 		intOutput = (intOutput << 2) | ((fec[i +(~j & 0x03)] >> (2 * ((j & 0x0C) >> 2))) & 0x03);
	 	}
	 
	 interleaved[i] = (intOutput >> 24) & 0xFF;
	 interleaved[i + 1] = (intOutput >> 16) & 0xFF;
	 interleaved[i + 2] = (intOutput >> 8) & 0xFF;
	 interleaved[i + 3] = (intOutput >> 0) & 0xFF;
	}

	for (i = 0; i < fecNum * 2; i++){
	 	pEncData[i] = interleaved[i];
	}

	/*
	printf("Interleaver output: [%5d bytes]\n", fecNum * 2);
	for (i = 0; i < fecNum * 2; i++){
	 	printf("%02X%s", interleaved[i], (i % 16 == 15) ? "\n" : (i % 4 == 3) ? " " : " ");
	}
	printf("\n\n");
	*/

	return fecNum * 2;
}

/**************************************************************************************************************
* @fn fecDecode
*
* @brief De-interleaves and decodes a given input buffer
*
* @param pDecData - Pointer to where to put decoded data (NULL when initializing at start of packet)
* pInData - Pointer to received data
* nRemBytes - of remaining (decoded) bytes to decode
*
*
* @return Number of bytes of decoded data stored at pDecData
*/
unsigned short fecDecode(unsigned char *pDecData, unsigned char* pInData, unsigned short nRemBytes)
{
 // Two sets of buffers (last, current) for each destination state for holding:
 static unsigned char nCost[2][8]; // Accumulated path cost
 static unsigned long aPath[2][8]; // Encoder input data (32b window)

 // Indices of (last, current) buffer for each iteration
 static unsigned char iLastBuf;
 static unsigned char iCurrBuf;

 // Number of bits in each path buffer
 static unsigned char nPathBits;

 // Variables used to hold # Viterbi iterations to run, # bytes output,
 // minimum cost for any destination state, bit index of input symbol
 unsigned char nIterations;
 unsigned short nOutputBytes = 0;
 unsigned char nMinCost;
 signed char iBit = 8 - 2;

 // Initialize variables at start of packet (and return without doing any more)
 if (pDecData == NULL) {
	 unsigned char n ;
	 memset(nCost, 0, sizeof(nCost));
	 for (n = 1; n < 8; n++)
	 nCost[0][n] = 100;
	 iLastBuf = 0;
	 iCurrBuf = 1;
	 nPathBits = 0;
	 return 0;
 }
 {
	 unsigned char aDeintData[4];
	 signed char iOut;
	 signed char iIn;

	 // De-interleave received data (and change pInData to point to de-interleaved data)
	 for (iOut = 0; iOut < 4; iOut++) {
		 unsigned char dataByte = 0;
		 for (iIn = 3; iIn >= 0; iIn--)
		 dataByte = (dataByte << 2) | ((pInData[iIn] >>( 2 * iOut)) & 0x03);
		 aDeintData[iOut] = dataByte;
	 }
	 pInData = aDeintData;
 }
 // Process up to 4 bytes of de-interleaved input data, processing one encoder symbol (2b) at a time
 for (nIterations = 16; nIterations > 0; nIterations--) {

	 unsigned char iDestState;
	 unsigned char symbol = ((*pInData) >> iBit) & 0x03;

	 // Find minimum cost so that we can normalize costs (only last iteration used)
	 nMinCost = 0xFF;

	 // Get 2b input symbol (MSB first) and do one iteration of Viterbi decoding
	 if ((iBit -= 2) < 0) {
		 iBit = 6;
		 pInData++; // Update pointer to the next byte of received data
	 }

	 // For each destination state in the trellis, calculate hamming costs for both possible paths into state and
	 // select the one with lowest cost.
	 for (iDestState = 0; iDestState < 8; iDestState++) {
		 unsigned char nCost0;
		 unsigned char nCost1;
		 unsigned char iSrcState0;
		 unsigned char iSrcState1;
		 unsigned char nInputBit;

		 nInputBit = aTrellisTransitionInput[iDestState];

		 // Calculate cost of transition from each of the two source states (cost is Hamming difference between
		 // received 2b symbol and expected symbol for transition)
		 iSrcState0 = aTrellisSourceStateLut[iDestState][0];
		 nCost0 = nCost[iLastBuf][iSrcState0];
		 nCost0 += hammWeight(symbol ^ aTrellisTransitionOutput[iDestState][0]);

		 iSrcState1 = aTrellisSourceStateLut[iDestState][1];
		 nCost1 = nCost[iLastBuf][iSrcState1];
		 nCost1 += hammWeight(symbol ^ aTrellisTransitionOutput[iDestState][1]); 

		  // Select transition that gives lowest cost in destination state, copy that source state's path and add
		 // new decoded bit
		 if (nCost0 <= nCost1) {
			 nCost[iCurrBuf][iDestState] = nCost0;
			 nMinCost = min(nMinCost, nCost0);
			 aPath[iCurrBuf][iDestState] = (aPath[iLastBuf][iSrcState0] << 1) | nInputBit;
		 } else {
			 nCost[iCurrBuf][iDestState] = nCost1;
			 nMinCost = min(nMinCost, nCost1);
			 aPath[iCurrBuf][iDestState] = (aPath[iLastBuf][iSrcState1] << 1) | nInputBit;
		 	}
	 }
	 nPathBits++;
	 // If trellis history is sufficiently long, output a byte of decoded data
	 if (nPathBits == 32) {
		 *pDecData++ = (aPath[iCurrBuf][0] >> 24) & 0xFF;
		 nOutputBytes++;
		 nPathBits -= 8;
		 nRemBytes--;
	 }

	 // After having processed 3-symbol trellis terminator, flush out remaining data
	 if ((nRemBytes <= 3) && (nPathBits == ((8 * nRemBytes) + 3))) {
		 while (nPathBits >= 8) {
			 *pDecData++ = (aPath[iCurrBuf][0] >> (nPathBits - 8)) & 0xFF;
			 nOutputBytes++;
			 nPathBits -= 8;
		 }
		return nOutputBytes;
	 }
	 // Swap current and last buffers for next iteration
	 iLastBuf = (iLastBuf + 1) % 2;
	 iCurrBuf = (iCurrBuf + 1) % 2;
 }

 // Normalize costs so that minimum cost becomes 0
 {
	 unsigned char iState;
	 for (iState = 0; iState < 8; iState++)
	 nCost[iLastBuf][iState] -= nMinCost;
 }
 return nOutputBytes;
}

/**************************************************************************************************************
* @fnprintbinchar
*
* @brief This function prints a character to binary
*
* @param None
*
* @return None
*/
//
void printbincharpad(char c)
{
    for (int i = 7; i >= 0; --i)
    {
        putchar( (c & (1 << i)) ? '1' : '0' );
    }
    //putchar('\n');
}

/**************************************************************************************************************
* @fn main
*
* @brief This code example demonstrates how the fecDecode function can be used. It is assumed that a
* flag, packetReceived, is asserted when a packet is received (there are 64 bytes in the RXFIFO)
*
* @param None
*
* @return None
*/
int main(int argc, char **argv)
{

	if(argc>2)
	{
		if(argv[1][0]=='e' || argv[1][0]=='d' || argv[1][0]=='f')
		{
			// switch for output data format
			if(argv[1][0]=='d')
			{
				mode_decode = 1;   // decode mode
				output_format = 0; // 0 for full datastream incl. preamble and sync word
			}
			else if(argv[1][0]=='f')
			{
				mode_decode = 1;   // decode mode
				output_format = 1; // 1 for preamble and sync word crop
			}

			if(argv[1][0]=='e')
			{
				mode_encode = 1;   //encode mode
			}

			/*
			printf("Str_len: %d\n",strlen(argv[2]));
			printf("Str.   : %s\n",argv[2]);
			int test = fromBinary(argv[2]);
			printf("HEX    : 0x%02X\n",test);
			printf("rx_byte: ");
			printf("\n");
			*/

			// fill the bitstream into bytes
			for(int i = 0; i < strlen(argv[2]); i++){
				
				if(argv[2][i] == '1')
				{
					rx_data[byte_count] = rx_data[byte_count] | (1<<(7-bit_count));
				}
				else if(argv[2][i] == '0')
				{
					//whatever
				}
				
				bit_count++;
				if(bit_count == 8)
				{
					bit_count = 0;
					byte_count++;
				}
			}
		}
	}

	/*
	printf("\n\nByte_Count: 0x%02X \n", byte_count);	

	printf("Array: ");
	for(int i=0; i<byte_count; i++){
		 printf("0x%02X ", rx_data[i]);
		 //printbincharpad(rx_data[i]);printf(" \n");
	}printf("\n");
	*/

//-----------------------------------[data received!]-----------------------------------------

	//Encoding variables
	unsigned char pktlen_byte = 1;
	unsigned char rssi_crc = 2;

	unsigned char fecEncData[512];
	unsigned char *pEncData = fecEncData;

	unsigned char NUMBER_OF_BYTES_AFTER_ENCODING;
    unsigned char NUMBER_OF_BYTES_BEFORE_ENCODING;


    //Decoding variables
    unsigned short checksum;
    unsigned short nBytes;
    unsigned char *pDecData = rxPacket;       // Destination for decoded data
    
    unsigned short count = 0;
    unsigned char premable_offset = 0;        //count of preamble bytes sent by the CC1101 ; default 4 
    unsigned char sync_offset = 0;            //count of syncword bytes sent by the CC1101 ; default 4 
 
    unsigned char NUMBER_OF_BYTES_BEFORE_DECODING;
    unsigned char NUMBER_OF_BYTES_AFTER_DECODING;
	 
    
    //automatic preamble offset detection
    while(rx_data[premable_offset] == 0xAA)   //0xAA is fixed preamble value
    {         
	 premable_offset++;
    }
    //printf("premable_len: %d\n",premable_offset);

    //automatic sync word offset detection
    if (premable_offset == 0) 
    {
    	sync_offset = 0; //no sync word and no preamble in setting
    }
    else if(rx_data[premable_offset] == rx_data[premable_offset+2] && rx_data[premable_offset+1] == rx_data[premable_offset+3])
    {
    	sync_offset = 4; //32-Bit sync word
    }
    else if(rx_data[premable_offset] != rx_data[premable_offset+2] && rx_data[premable_offset+1] != rx_data[premable_offset+3])
    {
    	sync_offset = 2; //16-Bit sync word
    }
    //printf("syncword_len: %d\n",sync_offset);

    


	if(mode_encode == 1)
	{
		/*
		* FEC Encoding Mode
		*/

		NUMBER_OF_BYTES_BEFORE_ENCODING = byte_count - (premable_offset + sync_offset + pktlen_byte + rssi_crc);     //byte count of non-FEC payload data excl. package len byte 


		NUMBER_OF_BYTES_AFTER_ENCODING = fecEncode(pEncData, rx_data+(premable_offset + sync_offset), NUMBER_OF_BYTES_BEFORE_ENCODING);

		/*
		for(unsigned char i=0;i<NUMBER_OF_BYTES_AFTER_ENCODING;i++){
			printf("%02X%s", pEncData[i], (i % 16 == 15) ? "\n" : (i % 4 == 3) ? " " : " ");
		}
		printf("\n");
		*/

		if(output_format == 0)  //output with preamble and sync word
		{	
			for(int i = 0; i < (premable_offset + sync_offset); i++) //output of preamble and syc word
		 	{
			 	printbincharpad(rx_data[i]);
	     	}
		}

		for(int i=0; i< NUMBER_OF_BYTES_AFTER_ENCODING; i++)          //output of data
		{
			printbincharpad(pEncData[i]);
	    }

		/*
		memcpy(rx_data + 8, pEncData, NUMBER_OF_BYTES_AFTER_ENCODING);

		for(unsigned char i=0;i< premable_offset + sync_offset + NUMBER_OF_BYTES_AFTER_ENCODING;i++){
			printf("%02X%s", rx_data[i], (i % 16 == 15) ? "\n" : (i % 4 == 3) ? " " : " ");
		}
		printf("\n");
		*/
	}
	else if(mode_decode == 1)
	{
		/*
		* FEC Decoding Mode
		*/

	    //calculates the byte count for FEC decoding
	    NUMBER_OF_BYTES_BEFORE_DECODING = byte_count - (premable_offset + sync_offset);     //byte count of encoded payload data incl. interleaving 
	    NUMBER_OF_BYTES_AFTER_DECODING = ((NUMBER_OF_BYTES_BEFORE_DECODING - 4) / 2 ) + 1;	//byte count of decoded payload
	    //printf("bytes before decoding: %d\n",NUMBER_OF_BYTES_BEFORE_DECODING);
	    //printf("bytes  after decoding: %d\n",NUMBER_OF_BYTES_AFTER_DECODING);

	    /*
		printf("Payload FEC encoded:");
		for(int i=0; i<NUMBER_OF_BYTES_BEFORE_DECODING; i++){
			printf("0x%02X ", rx_data[i + premable_offset + sync_offset]);
		}printf("\n");
		*/

		pDecData = rxPacket;

		// Perform de-interleaving and decoding (both done in the same function)
		fecDecode(NULL, NULL, 0); // The function needs to be called with a NULL pointer for
		 
		// initialization before every packet to decode
		nBytes = NUMBER_OF_BYTES_AFTER_DECODING;
		
		count = 0;

		//start the magic decoding
		while (nBytes > 0) {
			unsigned short nBytesOut;
			
			for(int i=0; i<4; i++){
				rxBuffer[i] = rx_data[count + (premable_offset + sync_offset)];   //+8 -> hide preamble and sync bytes if lenght is 4+4
				count++;
			}

			nBytesOut = fecDecode(pDecData, rxBuffer, nBytes);
			nBytes -= nBytesOut;
			pDecData += nBytesOut;
		}
		 
		/*
		printf("Payload FEC decoded:");
		for(int i=0; i<NUMBER_OF_BYTES_AFTER_DECODING; i++)
		{
		 	printf("0x%02X ", rxPacket[i]);
	    }printf("\n");
	    */
			
		//print output bitstream for urh 
		if(output_format == 0)  //output with preamble and sync word
		{	
			for(int i = 0; i < (premable_offset + sync_offset); i++) //output of preamble and syc word
		 	{
			 	printbincharpad(rx_data[i]);
	     	}
		}

		for(int i=0; i<NUMBER_OF_BYTES_AFTER_DECODING; i++)          //output of data
		{
			printbincharpad(rxPacket[i]);
	    }
	//----------------------------[finished]------------------------------------------------
	}

}
