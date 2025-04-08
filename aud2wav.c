
// aud2wav

// The main difference between AUD and IMA-ADPCM-WAV is that AUD contains a continuous stream of ADPCM data,
// decoder is never reinitialized, while WAV is divided into independently decodable blocks,
// each block starts with a header containing one decoded sample and decoder state

#include <stdio.h>
#include <stdlib.h> // errno
#include <unistd.h> // optarg, optind
#include <string.h>



/******************************** AUD headers ********************************/

// NEW AUD format header (bytes 0..11)
struct {
	unsigned short samplerate;
	unsigned short encsize_lo;
	unsigned short encsize_hi;
	unsigned short decsize_lo;
	unsigned short decsize_hi;
	unsigned char flags; // bit0=stereo, bit1=16bit
	unsigned char codec; // 1=Westwood ADPCM, 99=IMA ADPCM
} AUD_HEADER_NEW;

// OLD AUD format header (bytes 0..7)
struct {
	unsigned short samplerate;
	unsigned short encsize_lo;
	unsigned short encsize_hi;
	unsigned char flags; // bit0=stereo, bit1=16bit
	unsigned char codec; // 1=Westwood ADPCM, 99=IMA ADPCM
} AUD_HEADER_OLD;

// Version-independent pseudo header, used in this app only
struct {
	unsigned short samplerate;
	unsigned long encsize;
	unsigned long decsize; // not used
	unsigned char flags;
	unsigned char codec;
	// Additional file info
	unsigned int filesize;
	unsigned int first_block_offset;
	unsigned int first_block_size;
	unsigned int blocks;
	unsigned int adpcm_bytes;
	unsigned int num_samples;
} AUD_HEADER; // pseudo header

// Block header, follows file header (NEW bytes 12..19, OLD bytes 8..15)
struct {
	unsigned short encsize;
	unsigned short decsize;
	unsigned short deaf; // 0xDEAF
	unsigned short zero; // 0x0000
} AUD_BLOCK_HEADER;



/******************************** WAV headers ********************************/

// Full header of a PCM .wav file
struct {
	unsigned long RIFF;
	unsigned long riffsize;
	unsigned long WAVE;
	unsigned long fmt;
	unsigned long fmtlen;
	unsigned short wFormatTag; // 1=PCM
	unsigned short nChannels;
	unsigned long nSamplesPerSec;
	unsigned long nAvgBytesPerSec;
	unsigned short nBlockAlign;
	unsigned short wBitsPerSample;
	//unsigned short cbSize; // 0
	unsigned long data;
	unsigned long datalen;
} WAV_HEADER_PCM;

// Full header of an IMA ADPCM .wav file
struct {
	unsigned long RIFF;
	unsigned long riffsize;
	unsigned long WAVE;
	unsigned long fmt;
	unsigned long fmtlen;
	unsigned short wFormatTag; // 0x11=IMA ADPCM
	unsigned short nChannels;
	unsigned long nSamplesPerSec;
	unsigned long nAvgBytesPerSec;
	unsigned short nBlockAlign;
	unsigned short wBitsPerSample;
	unsigned short cbSize; // 2
	unsigned short samplesPerBlock;
	unsigned long fact;
	unsigned long factlen;
	unsigned long nSamples;
	unsigned long data;
	unsigned long datalen;
} WAV_HEADER_ADPCM;

// Header of each ADPCM block
struct {
	short sample; // PCM decoded sample
	unsigned char index; // decoder state initialization
	unsigned char zero;
} WAV_BLOCK_HEADER;



/******************************** ADPCM decoding ********************************/

// Lookup tables for algorithm #0

// Original code from EA 2025 source code release
// https://github.com/electronicarts/CnC_Remastered_Collection
// #include "ADPCM.CPP" // not used, included only for reference
#include "DTABLE.CPP" // long DiffTable[89 * 16]
#include "ITABLE.CPP" // unsigned short IndexTable[89 * 16]

// Lookup tables for algorithms #1, #2, #3

unsigned short ADPCM_STEP_TABLE[89] = {
	7,     8,     9,     10,    11,    12,     13,    14,    16,
	17,    19,    21,    23,    25,    28,     31,    34,    37,
	41,    45,    50,    55,    60,    66,     73,    80,    88,
	97,    107,   118,   130,   143,   157,    173,   190,   209,
	230,   253,   279,   307,   337,   371,    408,   449,   494,
	544,   598,   658,   724,   796,   876,    963,   1060,  1166,
	1282,  1411,  1552,  1707,  1878,  2066,   2272,  2499,  2749,
	3024,  3327,  3660,  4026,  4428,  4871,   5358,  5894,  6484,
	7132,  7845,  8630,  9493,  10442, 11487,  12635, 13899, 15289,
	16818, 18500, 20350, 22385, 24623, 27086,  29794, 32767
};
char ADPCM_INDEX_ADJUST[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

int use_algorithm = 0;

void ADPCM_decode_sample(char *index, long *sample, unsigned char nibble) {
	
	int diff;
	
	if (use_algorithm == 0) { // Algorithm #0: original Westwood, uses large pre-calculated lookup tables
		
		int fastindex = (*index << 4) + nibble;
		diff = DiffTable[fastindex];         // DTABLE.CPP
		*index = IndexTable[fastindex] >> 4; // ITABLE.CPP

	} else {
		
		// Code common to algorithms #1, #2, #3
		
		int sign = nibble & 8;
		int delta = nibble & 7;
		
		unsigned short step = ADPCM_STEP_TABLE[*index];
		
		switch (use_algorithm) {
			case 2:  // Algorithm #2: slightly optimized, not sample-accurate, error accumulates
				diff = ((delta * step) >> 2) + (step >> 3);
				break;
			
			case 3:  // Algorithm #3: fully optimized, even worse
				diff = ((delta * 2 + 1) * step) >> 3;
				break;
			
			default: // Algorithm #1: using small lookup tables, result is identical to the original
				diff = 0;
				if (delta & 4) diff += step; step >>= 1;
				if (delta & 2) diff += step; step >>= 1;
				if (delta & 1) diff += step; step >>= 1;
				diff += step;
		}
		
		if (sign) diff = -diff;
		
		*index += ADPCM_INDEX_ADJUST[delta];
		if (*index < 0) *index = 0;
		if (*index > 88) *index = 88;
		
	} // algorithms #1, #2, #3
	
	*sample += diff;
	if (*sample > 32767) *sample = 32767;
	if (*sample < -32768) *sample = -32768;
}



/******************************** THE PROGRAM ********************************/

void usage(char *argv0) {
	// Extract filename from the command line
	char *exe = strrchr(argv0, '/');      // Full path (unix)
	if (!exe) exe = strrchr(argv0, '\\'); // Full path (windows)
	exe = exe ? ++exe : argv0;            // Filename only
	
	fprintf(stderr, "Remuxes a Westwood AUD file into an IMA ADPCM WAV file\n", exe);
	fprintf(stderr, "Usage: %s [-o out1.wav] [-b <blocksize> | -d | -4] <input1.aud> [input2.aud ...]\n", exe);
	fprintf(stderr, "\t-o <filename>: specify first output filename, ignored if -4 is used\n");
	fprintf(stderr, "\t-b <blocksize>: specify WAV ADPCM block size (including header), possible values:\n");
	fprintf(stderr, "\t              512 - most compatible [default]\n");
	fprintf(stderr, "\t    8..2760 mod 4 - Windows ACM compatible\n");
	fprintf(stderr, "\t         4..32771 - all possible\n");
	fprintf(stderr, "\t               -1 - choose the smallest file out of ACM-compatible\n");
	fprintf(stderr, "\t               -2 - choose the smallest file out of all possible\n");
	fprintf(stderr, "\t-d: decode to PCM instead of remuxing\n");
	fprintf(stderr, "\t-4: decode to 4 PCM files using 4 different algorithms: (implies -d)\n");
	fprintf(stderr, "\t            algo0 - large LUT based, original Westwood [default]\n");
	fprintf(stderr, "\t            algo1 - small LUT based\n");
	fprintf(stderr, "\t            algo2 - small LUT based, slightly optimized\n");
	fprintf(stderr, "\t            algo3 - small LUT based, fully optimized\n");
	exit(0);
}

int main(int argc, char *argv[]) {
	
	// Default values for command-line input
	char *ofilename = 0;
	int blocksize = 512; // ADPCM bytes per block: 0..32767 -> Total bytes per block: 4..32771
	char decode = 0;
	int algo_last = 0; // set to 3 when decoding to 4 different algorithms
	
	// Parse command-line arguments
	int c;
	while ((c = getopt(argc, argv, "ho:b:d4")) != -1)
		switch (c) {
			case 'o': // output filename
				ofilename = optarg;
				break;
			
			case 'b': // blocksize
				c = atoi(optarg);
				if (((c >= 4) && (c <= 32771)) || (c == -1) || (c == -2)) {
					blocksize = c;
				} else
					fprintf(stderr, "Invalid blocksize specified: %d. Parameter ignored.\n", c);
				break;
			
			case 'd': // decode
				decode = 1;
				break;
			
			case '4': // decode x4
				decode = 1;
				algo_last = 3;
				break;
			
			default: // 'h', '?'
				usage(argv[0]);
		}
	
	if (argc == 1) // zero arguments passed
		usage(argv[0]);
	
	int arg;
	FILE *aud;
	FILE *wav;
	unsigned int b, i, o; // loop indexes: input block, input index, otput index
	unsigned int reat;
	unsigned char in_buffer[65535]; // absolute theoretical maximum
	unsigned char in_odd, out_odd, nibble;
	unsigned char out_buffer[sizeof(in_buffer) * 4];
	short *out_pcm = (short *)&out_buffer;
	char adpcm_index;
	long adpcm_sample;
	unsigned int wav_blocksize; // unlike "blocksize" this one does NOT include 4-byte header
	unsigned int wav_blocks;
	unsigned int wav_datalen;
	// These are used for finding optimal blocksize
	unsigned int test_blocks;
	unsigned int test_samples_per_block;
	unsigned int test_datalen;
	char *str;
	
	// Loop through all input files
	
	for (arg = optind; arg < argc; arg++) {
		aud = fopen(argv[arg], "rb"); 
		if (!aud) {
			fprintf(stderr, "Error opening %s: %s\n", argv[arg], strerror(errno));
		} else {
			fprintf(stderr, "\n%s: successfully opened\n", argv[arg]);
			
			fseek(aud, 0, SEEK_END);
			AUD_HEADER.filesize = ftell(aud);
			
			// Try to read block header at position after NEW format header
			
			fseek(aud, sizeof(AUD_HEADER_NEW), SEEK_SET);
			fread(&AUD_BLOCK_HEADER, 1, sizeof(AUD_BLOCK_HEADER), aud); // read block (bytes 12..19)
			if ((AUD_BLOCK_HEADER.deaf == 0xDEAF) && (AUD_BLOCK_HEADER.zero == 0)) {
				
				// Matched NEW format AUD
				
				fprintf(stderr, "New AUD format detected\n");
				AUD_HEADER.first_block_offset = sizeof(AUD_HEADER_NEW);
				fseek(aud, 0, SEEK_SET);
				fread(&AUD_HEADER_NEW, 1, sizeof(AUD_HEADER_NEW), aud);
				AUD_HEADER.samplerate = AUD_HEADER_NEW.samplerate;
				AUD_HEADER.encsize = AUD_HEADER_NEW.encsize_lo + (AUD_HEADER_NEW.encsize_hi << 16);
				AUD_HEADER.decsize = AUD_HEADER_NEW.decsize_lo + (AUD_HEADER_NEW.decsize_hi << 16);
				AUD_HEADER.flags = AUD_HEADER_NEW.flags;
				AUD_HEADER.codec = AUD_HEADER_NEW.codec;
			} else {
				
				// Try to read block header at position after OLD format header
				
				fseek(aud, sizeof(AUD_HEADER_OLD), SEEK_SET);
				fread(&AUD_BLOCK_HEADER, 1, sizeof(AUD_BLOCK_HEADER), aud);
				if ((AUD_BLOCK_HEADER.deaf == 0xDEAF) && (AUD_BLOCK_HEADER.zero == 0)) {
					
					// Matched OLD format AUD
					
					fprintf(stderr, "Old AUD format detected\n");
					AUD_HEADER.first_block_offset = sizeof(AUD_HEADER_OLD);
					fseek(aud, 0, SEEK_SET);
					fread(&AUD_HEADER_OLD, 1, sizeof(AUD_HEADER_OLD), aud);
					AUD_HEADER.samplerate = AUD_HEADER_OLD.samplerate;
					AUD_HEADER.encsize = AUD_HEADER_OLD.encsize_lo + (AUD_HEADER_OLD.encsize_hi << 16);
					AUD_HEADER.decsize = 0;
					AUD_HEADER.flags = AUD_HEADER_OLD.flags;
					AUD_HEADER.codec = AUD_HEADER_OLD.codec;
				} else {
					fprintf(stderr, "%s: unknown AUD format\n", argv[arg]);
					fclose(aud);
					continue;
				}
			}
			fprintf(stderr, "File size: %u\n", AUD_HEADER.filesize);
			fprintf(stderr, "Sample rate: %u\n", AUD_HEADER.samplerate);
			fprintf(stderr, "Encoded stream size: %u bytes\n", AUD_HEADER.encsize);
			if (AUD_HEADER.decsize)
				fprintf(stderr, "Decoded data size: %u bytes\n", AUD_HEADER.decsize);
			fprintf(stderr, "Flags: %s, %u-bit\n", (AUD_HEADER.flags & 1) ? "stereo" : "mono", (AUD_HEADER.flags & 2) ? 16 : 8);
			fprintf(stderr, "Codec: %u (%s)\n", AUD_HEADER.codec, (AUD_HEADER.codec == 1) ? "Westwood ADPCM" : (AUD_HEADER.codec == 99) ? "IMA ADPCM" : "Unknown");
			
			if (((AUD_HEADER.flags & 3) != 2) || (AUD_HEADER.codec != 99)) {
				fprintf(stderr, "Sorry, only mono 16-bit IMA ADPCM files are supported\n");
				fclose(aud);
				continue;
			}
			
			// Analyze AUD stream (first read-through), want to count blocks and samples in advance
			
			AUD_HEADER.blocks = 0;
			AUD_HEADER.adpcm_bytes = 0;
			while (1) {
				reat = fread(&AUD_BLOCK_HEADER, 1, sizeof(AUD_BLOCK_HEADER), aud);
				if (reat == 0) break; // end of file, OK
				if (reat != sizeof(AUD_BLOCK_HEADER)) {
					fprintf(stderr, "%s: error while analyzing file, read %u bytes of header instead of %u\n", argv[arg], reat, sizeof(AUD_BLOCK_HEADER));
					break;
				}
				if ((AUD_BLOCK_HEADER.deaf != 0xDEAF) || (AUD_BLOCK_HEADER.zero != 0)) {
					fprintf(stderr, "%s: error while analyzing file, invalid header @ offset %u\n", argv[arg], ftell(aud) - sizeof(AUD_BLOCK_HEADER));
					break;
				}
				reat = fread(in_buffer, 1, AUD_BLOCK_HEADER.encsize, aud);
				if (reat != AUD_BLOCK_HEADER.encsize) {
					fprintf(stderr, "%s: error while analyzing file, read %u bytes instead of %u\n", argv[arg], reat, AUD_BLOCK_HEADER.encsize);
					break;
				}
				if (AUD_HEADER.blocks == 0)
					AUD_HEADER.first_block_size = AUD_BLOCK_HEADER.encsize;
				
				AUD_HEADER.blocks++;
				AUD_HEADER.adpcm_bytes += AUD_BLOCK_HEADER.encsize;
			}
			AUD_HEADER.num_samples = AUD_HEADER.adpcm_bytes * 2;
			
			fprintf(stderr, "Scanned %u blocks, first block %u bytes, last block %u bytes\n", AUD_HEADER.blocks, AUD_HEADER.first_block_size, AUD_BLOCK_HEADER.encsize);
			i = AUD_HEADER.adpcm_bytes + sizeof(AUD_BLOCK_HEADER) * AUD_HEADER.blocks;
			fprintf(stderr, "Total ADPCM bytes with block headers: %u, diff with header: %d\n", i, i - AUD_HEADER.encsize);
			if (AUD_HEADER.decsize)
				fprintf(stderr, "Decoded PCM size: %u bytes, diff with header: %d\n", AUD_HEADER.num_samples * 2, AUD_HEADER.num_samples * 2 - AUD_HEADER.decsize);
			i = AUD_HEADER.num_samples * 1000 / AUD_HEADER.samplerate; // duration in milliseconds
			fprintf(stderr, "Duration: %u:%02u.%03u (%u samples)\n", i / 60000, (i / 1000) % 60, i % 1000, AUD_HEADER.num_samples);
			
			if (decode) {
				
				// -------------------------------- Mode 1: Decode AUD to PCM WAV --------------------------------
				
				for (use_algorithm = 0; use_algorithm <= algo_last; use_algorithm++) { // normally algo_last = 0 (if not -4)
					
					// Choose output filename if -o is not specified, if -4, or for second, third etc. input files
					
					if (!ofilename || algo_last || (arg != optind)) {
						ofilename = (char*)&out_buffer; // temporarily use wav output buffer
						strncpy(ofilename, argv[arg], sizeof(out_buffer) - 11); // leave space for ".algoX.wav\0"
						if (str = strrchr(ofilename, '.'))                 // if input filename has extension
							if (stricmp(str, ".aud") == 0)                 // and it is .aud
								str[0] = 0;                                // strip it
						str = &ofilename[strlen(ofilename)];               // find end of filename
						if (algo_last)                                     // if -4
							str += sprintf(str, ".algo%u", use_algorithm); // append .algoX
						strcpy(str, ".wav");                               // append .wav
					}
					
					wav = fopen(ofilename, "wb");
					if (!wav) {
						fprintf(stderr, "Error creating %s: %s\n", ofilename, strerror(errno));
					} else {
						
						fprintf(stderr, "Decoding AUD to %s\n", ofilename);
						
						WAV_HEADER_PCM.RIFF = 0x46464952;
						WAV_HEADER_PCM.riffsize = AUD_HEADER.num_samples * 2 + sizeof(WAV_HEADER_PCM) - 8;
						WAV_HEADER_PCM.WAVE = 0x45564157;
						WAV_HEADER_PCM.fmt = 0x20746D66;
						WAV_HEADER_PCM.fmtlen = 16;
						WAV_HEADER_PCM.wFormatTag = 1;
						WAV_HEADER_PCM.nChannels = 1;
						WAV_HEADER_PCM.nSamplesPerSec = AUD_HEADER.samplerate;
						WAV_HEADER_PCM.nAvgBytesPerSec = WAV_HEADER_PCM.nSamplesPerSec * 2;
						WAV_HEADER_PCM.nBlockAlign = 2;
						WAV_HEADER_PCM.wBitsPerSample = 16;
						WAV_HEADER_PCM.data = 0x61746164;
						WAV_HEADER_PCM.datalen = AUD_HEADER.num_samples * 2;
						
						reat = fwrite(&WAV_HEADER_PCM, 1, sizeof(WAV_HEADER_PCM), wav);
						if (reat != sizeof(WAV_HEADER_PCM)) {
							fprintf(stderr, "Error: wrote %d bytes of PCM WAV header instead of %d: %s\n", reat, sizeof(WAV_HEADER_PCM), strerror(errno));
							break;
						}
						
						// Initialize decoder
						adpcm_index = 0;
						adpcm_sample = 0;
						
						fseek(aud, AUD_HEADER.first_block_offset, SEEK_SET);
						
						// Decode all blocks
						for (b = 0; b < AUD_HEADER.blocks; b++) {
							fread(&AUD_BLOCK_HEADER, 1, sizeof(AUD_BLOCK_HEADER), aud);
							fread(in_buffer, 1, AUD_BLOCK_HEADER.encsize, aud);
							for (i = 0; i < AUD_BLOCK_HEADER.encsize; i++) {
								// Decode each byte into 2 samples, least significant nibble first
								ADPCM_decode_sample(&adpcm_index, &adpcm_sample, in_buffer[i] & 0xF);
								out_pcm[i*2] = adpcm_sample;
								ADPCM_decode_sample(&adpcm_index, &adpcm_sample, (in_buffer[i] >> 4) & 0xF);
								out_pcm[i*2 + 1] = adpcm_sample;
							}
							reat = fwrite(out_pcm, 1, AUD_BLOCK_HEADER.encsize * 4, wav);
							if (reat != AUD_BLOCK_HEADER.encsize * 4) {
								fprintf(stderr, "Error: wrote %d bytes of PCM WAV data instead of %d: %s\n", reat, AUD_BLOCK_HEADER.encsize * 4, strerror(errno));
								break;
							}
						} // for AUD blocks
						
						fclose(wav);
					} // if fopen(wav) succeeded
				} // for algorithms
				
			} else { // if remuxing (not decode)
				
				// -------------------------------- Mode 2: Remux AUD to ADPCM WAV --------------------------------
				
				// Choose output filename if -o is not specified, and for second, third etc. input files
				
				if (!ofilename || (arg != optind)) {
					ofilename = (char*)&out_buffer; // temporarily use wav output buffer
					strncpy(ofilename, argv[arg], sizeof(out_buffer) - 5); // leave space for ".wav\0"
					do {
						if (str = strrchr(ofilename, '.')) // if input filename has extension
							if (!stricmp(str, ".aud")) {   // and it is .aud
								strcpy(str, ".wav");       // replace it with .wav
								break;
							}
						strcat(ofilename, ".wav");         // otherwise append .wav
					} while(0);
				}
				
				wav = fopen(ofilename, "wb");
				if (!wav) {
					fprintf(stderr, "Error creating %s: %s\n", ofilename, strerror(errno));
				} else {
					
					fprintf(stderr, "Remuxing AUD to %s\n", ofilename);
					
					// Find optimal blocksize if needed
					// Initialize variables: wav_blocksize, wav_blocks, wav_datalen
					
					wav_datalen = INT_MAX;
					
					switch (blocksize) {
						
						case -1: // Find an ACM-compatible blocksize with smallest resulting file size, using bruteforce
							for (i = 8; i <= 2760; i += 4) {
								test_samples_per_block = (i - 4) * 2 + 1;
								test_blocks = AUD_HEADER.num_samples / test_samples_per_block;
								if (AUD_HEADER.num_samples % test_samples_per_block)
									test_blocks++;
								test_datalen = i * test_blocks;
								if (test_datalen < wav_datalen) {
									wav_blocksize = i - 4;
									wav_blocks = test_blocks;
									wav_datalen = test_datalen;
								}
							}
							break;
						
						case -2: // Find any blocksize with smallest resulting file size, using bruteforce
							for (i = 4; i <= 32771; i++) {
								test_samples_per_block = (i - 4) * 2 + 1;
								test_blocks = AUD_HEADER.num_samples / test_samples_per_block;
								if (AUD_HEADER.num_samples % test_samples_per_block)
									test_blocks++;
								test_datalen = i * test_blocks;
								if (test_datalen < wav_datalen) {
									wav_blocksize = i - 4;
									wav_blocks = test_blocks;
									wav_datalen = test_datalen;
								}
							}
							break;
						
						default: // Use default or user-specified blocksize
							wav_blocksize = blocksize - 4;
							test_samples_per_block = wav_blocksize * 2 + 1;
							wav_blocks = AUD_HEADER.num_samples / test_samples_per_block;
							if (AUD_HEADER.num_samples % test_samples_per_block)
								wav_blocks++;
							wav_datalen = blocksize * wav_blocks;
					}
					
					fprintf(stderr, "Selected WAV block size: %u (4 + %u) bytes\n", wav_blocksize + 4, wav_blocksize);
					
					WAV_HEADER_ADPCM.RIFF = 0x46464952;
					WAV_HEADER_ADPCM.riffsize = wav_datalen + sizeof(WAV_HEADER_ADPCM) - 8;
					WAV_HEADER_ADPCM.WAVE = 0x45564157;
					WAV_HEADER_ADPCM.fmt = 0x20746D66;
					WAV_HEADER_ADPCM.fmtlen = 20;
					WAV_HEADER_ADPCM.wFormatTag = 0x11;
					WAV_HEADER_ADPCM.nChannels = 1;
					WAV_HEADER_ADPCM.nSamplesPerSec = AUD_HEADER.samplerate;
					WAV_HEADER_ADPCM.nAvgBytesPerSec = AUD_HEADER.samplerate * (wav_blocksize + 4) / (wav_blocksize * 2 + 1);
					WAV_HEADER_ADPCM.nBlockAlign = wav_blocksize + 4;
					WAV_HEADER_ADPCM.wBitsPerSample = 4;
					WAV_HEADER_ADPCM.cbSize = 2;
					WAV_HEADER_ADPCM.samplesPerBlock = wav_blocksize * 2 + 1;
					WAV_HEADER_ADPCM.fact = 0x74636166;
					WAV_HEADER_ADPCM.factlen = 4;
					WAV_HEADER_ADPCM.nSamples = AUD_HEADER.num_samples;
					WAV_HEADER_ADPCM.data = 0x61746164;
					WAV_HEADER_ADPCM.datalen = wav_datalen;
					
					reat = fwrite(&WAV_HEADER_ADPCM, 1, sizeof(WAV_HEADER_ADPCM), wav);
					if (reat != sizeof(WAV_HEADER_ADPCM)) {
						fprintf(stderr, "Error: wrote %d bytes of ADPCM WAV header instead of %d: %s\n", reat, sizeof(WAV_HEADER_ADPCM), strerror(errno));
						break;
					}
					
					// Initialize decoder
					adpcm_index = 0;
					adpcm_sample = 0;
					
					fseek(aud, AUD_HEADER.first_block_offset, SEEK_SET);
					o = -1; // output byte index in WAV block, -1 means block header needs to be written instead
					
					// Loop through each AUD block
					for (b = 0; b < AUD_HEADER.blocks; b++) {
						fread(&AUD_BLOCK_HEADER, 1, sizeof(AUD_BLOCK_HEADER), aud);
						fread(in_buffer, 1, AUD_BLOCK_HEADER.encsize, aud);
						
						// Loop for each nibble in AUD block
						for (i = 0, in_odd = 0; i < AUD_BLOCK_HEADER.encsize; (in_odd = !in_odd) ? i : i++) {
							nibble = in_odd ? (in_buffer[i] >> 4) : (in_buffer[i] & 0xF);
							
							ADPCM_decode_sample(&adpcm_index, &adpcm_sample, nibble);
							
							if (o == -1) {
								// Write WAV block header
								WAV_BLOCK_HEADER.sample = adpcm_sample;
								WAV_BLOCK_HEADER.index = adpcm_index;
								WAV_BLOCK_HEADER.zero = 0;
								reat = fwrite(&WAV_BLOCK_HEADER, 1, sizeof(WAV_BLOCK_HEADER), wav);
								if (reat != sizeof(WAV_BLOCK_HEADER)) {
									fprintf(stderr, "Error: wrote %d bytes of ADPCM block header instead of %d: %s\n", reat, sizeof(WAV_BLOCK_HEADER), strerror(errno));
									b = AUD_HEADER.blocks; // breaks the outer loop of AUD blocks
									break;
								}
								// Prepare to write WAV block contents
								o = 0;
								out_odd = 0;
							} else { // o != -1
								// Put nibble into output buffer
								if (out_odd == 0) {
									out_buffer[o] = nibble;
								} else {
									out_buffer[o] += (nibble << 4);
									o++;
								}
								out_odd = !out_odd;
								
								// Write WAV block contents when block is full
								if (o == wav_blocksize) {
									reat = fwrite(out_buffer, 1, wav_blocksize, wav);
									if (reat != wav_blocksize) {
										fprintf(stderr, "Error: wrote %d bytes of ADPCM data instead of %d: %s\n", reat, wav_blocksize, strerror(errno));
										b = AUD_HEADER.blocks; // breaks the outer loop of AUD blocks
										break;
									}
									memset(out_buffer, 0, wav_blocksize);
									o = -1;
								}
							}
						}
					}
					
					// Write the last incomplete WAV block
					if (o != -1) {
						if (out_odd) o++;
						memset(&out_buffer[o], 0, wav_blocksize - o); // clear the rest of the buffer
						reat = fwrite(out_buffer, 1, wav_blocksize, wav);
						if (reat != wav_blocksize) {
							fprintf(stderr, "Error: wrote %d bytes of ADPCM data instead of %d: %s\n", reat, wav_blocksize, strerror(errno));
							break;
						}
					}
					
					fclose(wav);
					
				} // if fopen(wav) succeeded
			} // if remuxing
			
			fclose(aud);
			
		} // if fopen(aud) succeeded
	} // for all input files
  
  return 0;
}
