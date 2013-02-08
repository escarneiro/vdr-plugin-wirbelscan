/*
 * ttext.c: wirbelscan - A plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 * $Id$
 */

#include <time.h>
#include <vdr/config.h>
#include "ttext.h"
#include "countries.h"
#include "common.h"

#define TS_DBG  0
#define PES_DBG 0
#define CNI_DBG 0
#define FUZZY_DBG 0
#define MAXHITS 50
#define MINHITS 2
#define TIMEOUT 15
#define Dec(x)        (CharToInt(x) < 10)                  /* "z" == 0x30..0x39             */
#define Hex(x)        (CharToInt(x) < 16)                  /* "h" == 0x30..0x39; 0x41..0x46 */
#define Delim(x)      ((x==46) || (x==58))                 /* date/time delimiter.          */
#define Magenta(x)    ((x==5))                             /* ":" == 0x05                   */
#define Concealed(x)  ((x==0x1b) || (x==0x18))             /* "%" == 0x18; 300231 has typo. */


uchar Revert8(uchar inp) {
  uint8_t lookup[] = {
      0, 8, 4, 12, 2, 10, 6, 14,
      1, 9, 5, 13, 3, 11, 7, 15 };
  return lookup[(inp & 0xF0) >> 4] | lookup[inp & 0xF] << 4;
}

uint16_t Revert16(uint16_t inp) {
  return Revert8(inp & 0xFF) << 8 | Revert8(inp >> 8);
}

uchar RevertNibbles(uchar inp) {
   inp = Revert8(inp);
   return (inp & 0xF0) >> 4 | (inp & 0xF) << 4;
}

uchar Hamming_8_4(uchar aByte) {
   uchar lookup[] = {
       21,   2,  73,  94, 100, 115,  56,  47,
      208, 199, 140, 155, 161, 182, 253, 234 };
   return lookup[aByte];
}

int HammingDistance_8(uchar a, uchar b) {
   int i, ret = 0;
   int mask[] = {1,2,4,8,16,32,64,128};
   uchar XOR  = a ^ b;
   
   for (i=0; i<8; i++)
       ret += (XOR & mask[i]) >> i;
   return ret;
}

uchar OddParity(uchar u) {
   int i;
   int mask[] = {1,2,4,8,16,32,64,128};
   uchar p=0;

   for (i=0; i<8; i++)
       p += (u & mask[i]) >> i;

   return p&1?u&0x7F:255;
}

int DeHamming_8_4(uchar aByte) {
  
   for (int i=0; i<16; i++)
       if (HammingDistance_8(Hamming_8_4(i),aByte) < 2)
          return i;

   return -1; // 2 or more bit errors: non recoverable.
}

uint32_t DeHamming_24_18(uchar * data) {
  //FIXME: add bit error detection and correction.
  return ((*(data + 0) >> 2) & 0x1) |
         ((*(data + 0) >> 3) & 0xE) |
         (uint32_t) ((*(data+1) & 0x7F) << 4 ) |
         (uint32_t) ((*(data+2) & 0x7F) << 11);
}

static inline uint8_t CharToInt(uchar c) {
   switch(c) {
      case 0x30 ... 0x39: return c-48;
      case 0x41 ... 0x46: return c-55;
      case 0x61 ... 0x66: return c-87;
      default: return 0xff;
      }
}

/*-----------------------------------------------------------------------------
 * cSwReceiver
 *---------------------------------------------------------------------------*/

void cSwReceiver::Receive(uchar * Data, int Length) {
  uchar * p;
  int count = 184;
  
  if (stopped || !Running()) return;
 
  // pointer to new PES header or next 184byte payload of current PES packet
  p = &Data[4];

  if (*(p+0) == 0 && *(p+1) == 0 && *(p+2) == 1) { // next PES header
      uchar PES_header_data_length = *(p+8);
      uchar data_identifier;      

      data_identifier = *(p+9+PES_header_data_length);
      if ((data_identifier < 0x10) || (data_identifier>0x1F))
         return; // unknown EBU data. not stated in 300472
      p+=10+PES_header_data_length; count-=10+PES_header_data_length;   
      }

  buffer->Put(p, count);
}

void cSwReceiver::DecodePacket(uchar * data) {
  uchar MsbFirst[48] = { 0 };
  uchar aByte;
  int i;
  int Magazine = 0;
  int PacketNumber = 0;
  static uint16_t pagenumber, subpage;
  
  switch (data[0]) { // data_unit_id
      case 0xC3: // VPS line 16
         {
         uchar * p = &MsbFirst[3];
         
         /* convert data from lsb first to msb first */
         for (i = 3; i<16; i++)     
             MsbFirst[i]=Revert8(data[i]);
         
         cni_vps = (p[10] & 0x03) << 10 |
                   (p[11] & 0xC0) << 2  |
                   (p[ 8] & 0xC0)       |
                   (p[11] & 0x3F);
         
         if (cni_vps && GetCniNameVPS()) {
            if (CNI_DBG)
               dlog(3, "cni_vps = 0x%.3x -> %s", cni_vps, GetCniNameVPS());
            if (++hits > MAXHITS) stopped = true;
            }
         }
         break;

      case 0x02: // EBU teletext
         {
         /* convert data from lsb first to msb first */
         for (i = 4; i<46; i++)
             MsbFirst[i]=Revert8(data[i]);

         /* decode magazine and packet number */
         aByte = DeHamming_8_4(MsbFirst[4]);

         if (! (Magazine = aByte & 0x7)) Magazine = 8;

         PacketNumber = (aByte & 0xF) >> 3 | DeHamming_8_4(MsbFirst[5]) << 1;

         switch (PacketNumber) {
            case 0: /* page header X/0 */
               {             
               uchar * p = &MsbFirst[6];               
               pagenumber = DeHamming_8_4(*(p)) + 10 * DeHamming_8_4(*(p+1)); p += 2;
               subpage  =  DeHamming_8_4(*(p++));
               subpage |= (DeHamming_8_4(*(p++)) & 0x7) << 4;
               subpage |=  DeHamming_8_4(*(p++)) << 7;
               subpage |= (DeHamming_8_4(*(p++)) & 0x3) << 11;
               if (Magazine == 1 && ! pagenumber) {
                  int bytes = 32;
                  p = &MsbFirst[14];
                  for (int i = 0; i<bytes; i++) *(p+i) &= 0x7F;
                  if (FUZZY_DBG) hexdump("raw data", p, bytes);
                  for (int i = 0; i<bytes-2; i++) {
                      if ((p[i] == 49) && (p[i+1] == 48) && (p[i+2] == 48)) {
                         p[i] = p[i+1] = p[i+2] = 0;
                         break;
                         }
                      }
                  for (int i = 0; i<bytes-4; i++) {
                      if ((p[i  ] == 118 || p[i  ] == 86) &&
                          (p[i+1] == 105 || p[i+1] == 73) &&
                          (p[i+2] == 100 || p[i+2] == 68) &&
                          (p[i+3] == 101 || p[i+3] == 69) &&
                          (p[i+4] == 111 || p[i+4] == 79))
                         p[i] = p[i+1] = p[i+2] = p[i+3] = p[i+4] = 0;

                      if ((p[i  ] == 116 || p[i  ] == 84) &&
                          (p[i+1] == 101 || p[i+1] == 69) &&
                          (p[i+2] == 120 || p[i+2] == 88) &&
                          (p[i+3] == 116 || p[i+3] == 84))
                         p[i] = p[i+1] = p[i+2] = p[i+3] = 0;   

                      if (Dec(p[i]) && Dec(p[i+1]) && Delim(p[i+2]))
                         p[i] = p[i+1] = p[i+2] = 0;
                      }

                  for (int i = 0; i<bytes-3; i++) {
                      if ((p[i] <= 32) && (p[i+1] <= 32)) {
                         p[i] = p[i+1] = 0;
                         }

                      if (p[i] < 32)
                         p[i] = 0;

                      if (((p[i] == 45) || (p[i] == 46)) && (p[i+1] < 32))
                         p[i] = 0;
                      }

                  while (*p < 33) { p++; bytes--; }
                  while (*(p + bytes) < 33) bytes--;
                  if (FUZZY_DBG) hexdump("fuzzy data", p, bytes);
                  UpdatefromName((const char *) p);
                  }
               }
               break;
            case 1 ... 25: /* visible text X/1..X/25 */
               {
               /* PDC embedded into visible text.
                * 300231 page 30..31 'Method A'
                */
               uchar * p = &MsbFirst[6];
               if (Magazine != 3) break;
               for (int i = 0; i<48; i++) MsbFirst[i] &= 0x7F;
               for (int i=0; i<34; i++) {
                   if (Magenta(*(p++))) {
                      if (Concealed(*(p)) && Hex(*(p+1)) && Hex(*(p+2)) && Dec(*(p+3)) && Dec(*(p+4)) && Dec(*(p+5)) &&
                         Concealed(*(p+6))) {
                         cni_cr_idx =  CharToInt(*(p+1)) << 12 | CharToInt(*(p+2)) << 8;
                         cni_cr_idx |= CharToInt(*(p+3)) * 100 + CharToInt(*(p+4)) * 10 + CharToInt(*(p+5));
                         if (cni_cr_idx && GetCniNameCrIdx()) {
                            if (CNI_DBG)
                               dlog(3, "cni_cr_idx = %.2X%.3d -> %s",
                                    cni_cr_idx>>8, cni_cr_idx&255, GetCniNameCrIdx());
                            if (++hits > MAXHITS) stopped = true;
                            }
                         }
                      }
                   }
               }
               break;

            case 26:
               {               
               /* 13 x 3-byte pairs of hammed 24/18 data.
                * 300231 page 30..31 'Method B'
                */
               uchar * p = &MsbFirst[7];
               for (int i=0; i<13; i++) {
                   uint32_t value = DeHamming_24_18(p); p+=3;
                   uint16_t data_word_A, mode_description, data_word_B;
                   
                   data_word_A      = (value >> 0 ) & 0x3F;
                   mode_description = (value >> 6 ) & 0x1F;
                   data_word_B      = (value >> 11) & 0x7F;
                   if (mode_description == 0x08) {
                       // 300231, 7.3.2.3 Coding of preselection data in extension packets X/26
                       if (data_word_A >= 0x30) {
                           cni_X_26 = data_word_A << 8 | data_word_B;
                           if (cni_X_26 && GetCniNameX26()) {
                              if (CNI_DBG)
                                 dlog(3, "cni_X_26 = 0x%.4x -> %s", cni_X_26, GetCniNameX26());
                              if (++hits > MAXHITS) stopped = true;
                              }
                           }
                       }
                   }
               }
               break; // end X/26
            case 30:
               if (Magazine < 8) break;
               {
               int DesignationCode = DeHamming_8_4(MsbFirst[6]);
               switch (DesignationCode) {
                   case 0:
                   case 1:
                      /* ETS 300 706: Table 18: Coding of Packet 8/30 Format 1 */
                      {
                      cni_8_30_1 = Revert16(MsbFirst[13] | MsbFirst[14] << 8);
                      if ((cni_8_30_1 == 0xC0C0) || (cni_8_30_1 == 0xA101))
                         cni_8_30_1 = 0; // known wrong values.
                     
                      if (cni_8_30_1 && GetCniNameFormat1())
                         if (CNI_DBG)
                            dlog(3, "cni_8_30_1 = 0x%.4x -> %s", cni_8_30_1, GetCniNameFormat1());
                         if (++hits > MAXHITS) stopped = true;
                      }
                      break;
                   case 2:
                   case 3:
                      /* ETS 300 706: Table 19: Coding of Packet 8/30 Format 2 */
                      {
                      uchar Data[256];
                      unsigned CNI_0, CNI_1, CNI_2, CNI_3;

                      for (i=0; i<33; i++)
                          Data[i]=RevertNibbles(DeHamming_8_4(MsbFirst[13+i]));

                      CNI_0 = (Data[2] & 0xF) >> 0;
                      CNI_2 = (Data[3] & 0xC) >> 2;
                      CNI_1 = (Data[8] & 0x3) << 2 | (Data[9]  & 0xC) >> 2;
                      CNI_3 = (Data[9] & 0x3) << 4 | (Data[10] & 0xF) >> 0;

                      cni_8_30_2 = CNI_0 << 12 | CNI_1 << 8 | CNI_2 << 6 | CNI_3; 
                      
                      if (cni_8_30_2 && GetCniNameFormat2())
                         if (CNI_DBG)
                            dlog(3, "cni_8_30_2 = 0x%.4x -> %s", cni_8_30_2, GetCniNameFormat2());
                         if (++hits > MAXHITS) stopped = true;
                      }
                      break;
                   default:;
                   } // end switch (DesignationCode)
               }
               break; // end 8/30/{1,2}
            default:;
            }
         }
         break; // end EBU teletext

      default:; // ignore wss && closed_caption
      } // end switch data_unit_id
}

void cSwReceiver::Decode(uchar * data, int count) {
  if (count < 184) return;

  while (count >= 46 && !stopped) { 
    switch (*data) {  // EN 300472 Table 4 data_unit_id
        case 0x02:    // EBU teletext 
        case 0xC3:    // VPS line 16
           DecodePacket(data);
        default:;     // wss, closed_caption, stuffing
        }
    data += 46; count -= 46; buffer->Del(46);
    }
}

void cSwReceiver::Action() {
  uchar * bp;
  int count;

  while (Running() & !stopped) {
     count = 184;
     
     if ((bp = buffer->Get(count))) {
        Decode(bp, count);      // always N * 46bytes.
        TsCount++;
        }
     else {
        cCondWait::SleepMs(10); // wait for new data.
        }
     if (time(0) > timeout) {        
        stopped = true;
        }
     }
  if (hits < MAXHITS) {
     // we're unshure about result.
     if ((hits < MINHITS) || (TsCount < 3)) {
        // probably garbage. clear it.
        cni_8_30_1 = cni_8_30_2 = cni_X_26 = cni_vps = cni_cr_idx = 0;
        fuzzy = false;
        }
     else
        dlog(3, "   fuzzy result, hits = %d, TsCount = %lu", hits, TsCount);   
     }
}

cSwReceiver::cSwReceiver(cChannel * Channel) : cReceiver(Channel, 100), cThread("ttext") {

   AddPid(Channel->Tpid());
   stopped = fuzzy = false;
   channel = Channel;
   buffer  = new cRingBufferLinear(MEGABYTE(1),184);
   cni_8_30_1 = cni_8_30_2 = cni_X_26 = cni_vps = cni_cr_idx = 0;
   hits = 0;
   timeout = time(0) + TIMEOUT;
}

void cSwReceiver::Reset() {
   stopped = fuzzy = false;
   cCondWait::SleepMs(10);
   buffer->Clear();
   cni_8_30_1 = cni_8_30_2 = cni_X_26 = cni_vps = cni_cr_idx = 0;
   stopped = false;
   hits = TsCount = 0;
   timeout = time(0) + TIMEOUT;
   Start();   
}

cSwReceiver::~cSwReceiver() {
   stopped = true;
   buffer->Clear();
   DELETENULL(buffer);
   Cancel(0);
}


/*-----------------------------------------------------------------------------
 * CNI codes
 *---------------------------------------------------------------------------*/

using namespace COUNTRY;

typedef struct {
  enum country_t id;
  const char * network;
  uint16_t ni_8_30_1;
  uint8_t  c_8_30_2;
  uint8_t  ni_8_30_2;
  uint8_t  a_X_26;
  uint8_t  b_X_26;
  uint16_t vps__cni;
  uint16_t cr_idx;
} cni_code;


static const cni_code cni_codes[] = {
/*Country, Network,                     8/30/1   8/30/2      X/26      VPS CNI cr_idx
 *                                      NI 16b   C 8b NI 8b  A 6b  B 7b
 */
 { GB, "CNNI"                          ,  0x1F2, 0x5B, 0xF1, 0x3B, 0x71, 0,     0   },
 { CR, "HRT1"                          ,  0x385, 0,    0,    0,    0,    0,     0   },
 { CR, "NovaTV"                        ,  0x386, 0,    0,    0,    0,    0,     0   },
 { CR, "HRT2"                          ,  0x387, 0,    0,    0,    0,    0,     0   },
 { CR, "HRT PLUS"                      ,  0x388, 0,    0,    0,    0,    0,     0   },
 { CR, "RTL Televizija"                ,  0x400, 0,    0,    0,    0,    0,     0   },
 { CR, "RTL PLUS"                      ,  0x401, 0,    0,    0,    0,    0,     0   },
 { CR, "TV Nova"                       ,  0x402, 0,    0,    0,    0,    0,     0   },
 { CR, "RI-TV"                         ,  0x403, 0,    0,    0,    0,    0,     0   },
 { BE, "VT4"                           ,  0x404, 0x16, 0x4,  0x36, 0x4,  0,     0   },
 { CR, "Kanal RI"                      ,  0x405, 0,    0,    0,    0,    0,     0   },
 { CR, "NIT"                           ,  0x406, 0,    0,    0,    0,    0,     0   },
 { CR, "Kapital Network"               ,  0x407, 0,    0,    0,    0,    0,     0   },
 { CR, "Vox TV Zadar"                  ,  0x408, 0,    0,    0,    0,    0,     0   },
 { CR, "GTV Zadar"                     ,  0x409, 0,    0,    0,    0,    0,     0   },
 { CR, "TV Sibenik"                    ,  0x40A, 0,    0,    0,    0,    0,     0   },
 { CR, "VTV Varazdin"                  ,  0x40B, 0,    0,    0,    0,    0,     0   },
 { CR, "TV Cakovec"                    ,  0x40C, 0,    0,    0,    0,    0,     0   },
 { CR, "TV Jadran"                     ,  0x40D, 0,    0,    0,    0,    0,     0   },
 { CR, "Z1"                            ,  0x40E, 0,    0,    0,    0,    0,     0   },
 { CR, "OTV"                           ,  0x40F, 0,    0,    0,    0,    0,     0   },
 { CH, "Schweizer Sportfernsehen"      ,  0x49A, 0,    0,    0,    0,    0,     0   },
 { GB, "MERIDIAN"                      , 0x10E4, 0x2C, 0x34, 0x3C, 0x34, 0,     0   },
 { GB, "CHANNEL 5 (2)"                 , 0x1609, 0x2C, 0x9,  0x3C, 0x9,  0,     0   },
 { GB, "COMEDY CENTRAL"                , 0x1D89, 0,    0,    0,    0,    0,     0   }, /* hmm.., GB or DE?  */
 { GB, "WESTCOUNTRY TV"                , 0x25D0, 0x2C, 0x30, 0x3C, 0x30, 0,     0   },
 { GB, "WESTCOUNTRY"                   , 0x25D1, 0x5B, 0xE8, 0x3B, 0x68, 0,     0   },
 { GB, "WESTCOUNTRY"                   , 0x25D2, 0x5B, 0xE9, 0x3B, 0x69, 0,     0   },
 { GB, "CHANNEL 5 (3)"                 , 0x28EB, 0x2C, 0x2B, 0x3C, 0x2B, 0,     0   },
 { GB, "CENTRAL TV"                    , 0x2F27, 0x2C, 0x37, 0x3C, 0x37, 0,     0   },
 { GR, "ET-1"                          , 0x3001, 0x21, 0x1,  0x31, 0x1,  0,     0   },
 { GR, "NET"                           , 0x3002, 0x21, 0x2,  0x31, 0x2,  0,     0   },
 { GR, "ET-3"                          , 0x3003, 0x21, 0x3,  0x31, 0x3,  0,     0   },
 { GR, "ET"                            , 0x3004, 0x21, 0x4,  0x31, 0x4,  0,     0   },
 { GR, "ET"                            , 0x3005, 0x21, 0x5,  0x31, 0x5,  0,     0   },
 { GR, "ET"                            , 0x3006, 0x21, 0x6,  0x31, 0x6,  0,     0   },
 { GR, "ET"                            , 0x3007, 0x21, 0x7,  0x31, 0x7,  0,     0   },
 { GR, "ET"                            , 0x3008, 0x21, 0x8,  0x31, 0x8,  0,     0   },
 { GR, "ET"                            , 0x3009, 0x21, 0x9,  0x31, 0x9,  0,     0   },
 { GR, "ET"                            , 0x300A, 0x21, 0xA,  0x31, 0xA,  0,     0   },
 { GR, "ET"                            , 0x300B, 0x21, 0xB,  0x31, 0xB,  0,     0   },
 { GR, "ET"                            , 0x300C, 0x21, 0xC,  0x31, 0xC,  0,     0   },
 { GR, "ET"                            , 0x300D, 0x21, 0xD,  0x31, 0xD,  0,     0   },
 { GR, "ET"                            , 0x300E, 0x21, 0xE,  0x31, 0xE,  0,     0   },
 { GR, "ET"                            , 0x300F, 0x21, 0xF,  0x31, 0xF,  0,     0   },
 { DE, "RTL Television"                , 0x3100, 0,    0,    0,    0,    0,     0   }, /* hmmm..., 3100?? */
 { NL, "Nederland 1"                   , 0x3101, 0x48, 0x1,  0x38, 0x1,  0,     0   },
 { NL, "Nederland 2"                   , 0x3102, 0x48, 0x2,  0x38, 0x2,  0,     0   },
 { NL, "Nederland 3"                   , 0x3103, 0x48, 0x3,  0x38, 0x3,  0,     0   },
 { NL, "RTL 4"                         , 0x3104, 0x48, 0x4,  0x38, 0x4,  0,     0   },
 { NL, "RTL 5"                         , 0x3105, 0x48, 0x5,  0x38, 0x5,  0,     0   },
 { NL, "NPO"                           , 0x3107, 0x48, 0x7,  0x38, 0x7,  0,     0   },
 { NL, "NPO"                           , 0x3108, 0x48, 0x8,  0x38, 0x8,  0,     0   },
 { NL, "NPO"                           , 0x3109, 0x48, 0x9,  0x38, 0x9,  0,     0   },
 { NL, "NPO"                           , 0x310A, 0x48, 0xA,  0x38, 0xA,  0,     0   },
 { NL, "NPO"                           , 0x310B, 0x48, 0xB,  0x38, 0xB,  0,     0   },
 { NL, "NPO"                           , 0x310C, 0x48, 0xC,  0x38, 0xC,  0,     0   },
 { NL, "NPO"                           , 0x310D, 0x48, 0xD,  0x38, 0xD,  0,     0   },
 { NL, "NPO"                           , 0x310E, 0x48, 0xE,  0x38, 0xE,  0,     0   },
 { NL, "NPO"                           , 0x310F, 0x48, 0xF,  0x38, 0xF,  0,     0   },
 { NL, "RTV Noord"                     , 0x3110, 0,    0,    0,    0,    0,     0   },
 { NL, "Omrop Fryslan"                 , 0x3111, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Drenthe"                   , 0x3112, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Oost"                      , 0x3113, 0,    0,    0,    0,    0,     0   },
 { NL, "Omroep Gelderland"             , 0x3114, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x3115, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Noord-Holland"             , 0x3116, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Utrecht"                   , 0x3117, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV West"                      , 0x3118, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Rijnmond"                  , 0x3119, 0,    0,    0,    0,    0,     0   },
 { NL, "Omroep Brabant"                , 0x311A, 0,    0,    0,    0,    0,     0   },
 { NL, "L1 RTV Limburg"                , 0x311B, 0,    0,    0,    0,    0,     0   },
 { NL, "Omroep Zeeland"                , 0x311C, 0,    0,    0,    0,    0,     0   },
 { NL, "Omroep Flevoland"              , 0x311D, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x311E, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x311F, 0,    0,    0,    0,    0,     0   },
 { NL, "The BOX"                       , 0x3120, 0x48, 0x20, 0x38, 0x20, 0,     0   },
 { NL, "Discovery Netherlands"         , 0x3121, 0,    0,    0,    0,    0,     0   },
 { NL, "Nickelodeon"                   , 0x3122, 0x48, 0x22, 0x38, 0x22, 0,     0   },
 { NL, "Animal Planet Benelux"         , 0x3123, 0,    0,    0,    0,    0,     0   },
 { NL, "TIEN"                          , 0x3124, 0,    0,    0,    0,    0,     0   },
 { NL, "NET5"                          , 0x3125, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS6"                          , 0x3126, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x3127, 0,    0,    0,    0,    0,     0   },
 { NL, "V8"                            , 0x3128, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x3129, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312A, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312B, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312C, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312D, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312E, 0,    0,    0,    0,    0,     0   },
 { NL, "SBS"                           , 0x312F, 0,    0,    0,    0,    0,     0   },
 { NL, "TMF (Netherlands service)"     , 0x3130, 0,    0,    0,    0,    0,     0   },
 { NL, "TMF (Belgian Flanders service)", 0x3131, 0,    0,    0,    0,    0,     0   },
 { NL, "MTV NL"                        , 0x3132, 0,    0,    0,    0,    0,     0   },
 { NL, "Nickelodeon"                   , 0x3133, 0,    0,    0,    0,    0,     0   },
 { NL, "The Box"                       , 0x3134, 0,    0,    0,    0,    0,     0   },
 { NL, "TMF Pure"                      , 0x3135, 0,    0,    0,    0,    0,     0   },
 { NL, "TMF Party"                     , 0x3136, 0,    0,    0,    0,    0,     0   },
 { NL, "RNN7"                          , 0x3137, 0,    0,    0,    0,    0,     0   },
 { NL, "TMF NL"                        , 0x3138, 0,    0,    0,    0,    0,     0   },
 { NL, "Nick Toons"                    , 0x3139, 0,    0,    0,    0,    0,     0   },
 { NL, "Nick Jr."                      , 0x313A, 0,    0,    0,    0,    0,     0   },
 { NL, "Nick Hits"                     , 0x313B, 0,    0,    0,    0,    0,     0   },
 { NL, "MTV Brand New"                 , 0x313C, 0,    0,    0,    0,    0,     0   },
 { NL, "Disney Channel"                , 0x313D, 0,    0,    0,    0,    0,     0   },
 { NL, "Disney XD"                     , 0x313E, 0,    0,    0,    0,    0,     0   },
 { NL, "Playhouse Disney"              , 0x313F, 0,    0,    0,    0,    0,     0   },
 { NL, "RTL 7"                         , 0x3147, 0x48, 0x47, 0x38, 0x47, 0,     0   },
 { NL, "RTL 8"                         , 0x3148, 0x48, 0x48, 0x38, 0x48, 0,     0   },
 { NL, "HET GESPREK"                   , 0x3150, 0,    0,    0,    0,    0,     0   },
 { NL, "InfoThuis TV"                  , 0x3151, 0,    0,    0,    0,    0,     0   },
 { NL, "Graafschap TV(Oost-Gelderland)", 0x3152, 0,    0,    0,    0,    0,     0   },
 { NL, "Gelre TV (Veluwe)"             , 0x3153, 0,    0,    0,    0,    0,     0   },
 { NL, "Gelre TV (Groot-Arnhem)"       , 0x3154, 0,    0,    0,    0,    0,     0   },
 { NL, "Gelre TV (Groot-Nijmegen)"     , 0x3155, 0,    0,    0,    0,    0,     0   },
 { NL, "Gelre TV (Betuwe)"             , 0x3156, 0,    0,    0,    0,    0,     0   },
 { NL, "Brabant 10 (West Brabant)"     , 0x3157, 0,    0,    0,    0,    0,     0   },
 { NL, "Brabant 10 (Midden Brabant)"   , 0x3158, 0,    0,    0,    0,    0,     0   },
 { NL, "Brabant 10(Noord Oost Brabant)", 0x3159, 0,    0,    0,    0,    0,     0   },
 { NL, "Brabant 10 (Zuid Oost Brabant)", 0x315A, 0,    0,    0,    0,    0,     0   },
 { NL, "Regio22"                       , 0x315B, 0,    0,    0,    0,    0,     0   },
 { NL, "Maximaal TV"                   , 0x315C, 0,    0,    0,    0,    0,     0   },
 { NL, "GPTV"                          , 0x315D, 0,    0,    0,    0,    0,     0   },
 { NL, "1TV (Groningen)"               , 0x315E, 0,    0,    0,    0,    0,     0   },
 { NL, "1TV (Drenthe)"                 , 0x315F, 0,    0,    0,    0,    0,     0   },
 { NL, "Cultura 24"                    , 0x3160, 0,    0,    0,    0,    0,     0   },
 { NL, "101 TV"                        , 0x3161, 0,    0,    0,    0,    0,     0   },
 { NL, "Best 24"                       , 0x3162, 0,    0,    0,    0,    0,     0   },
 { NL, "Holland Doc 24"                , 0x3163, 0,    0,    0,    0,    0,     0   },
 { NL, "Geschiedenis 24"               , 0x3164, 0,    0,    0,    0,    0,     0   },
 { NL, "Consumenten 24"                , 0x3165, 0,    0,    0,    0,    0,     0   },
 { NL, "Humor 24"                      , 0x3166, 0,    0,    0,    0,    0,     0   },
 { NL, "Sterren 24"                    , 0x3167, 0,    0,    0,    0,    0,     0   },
 { NL, "Spirit 24"                     , 0x3168, 0,    0,    0,    0,    0,     0   },
 { NL, "Familie 24"                    , 0x3169, 0,    0,    0,    0,    0,     0   },
 { NL, "Journaal 24"                   , 0x316A, 0,    0,    0,    0,    0,     0   },
 { NL, "Politiek 24"                   , 0x316B, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x316C, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x316D, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x316E, 0,    0,    0,    0,    0,     0   },
 { NL, "NPO"                           , 0x316F, 0,    0,    0,    0,    0,     0   },
 { NL, "RTV Rotterdam"                 , 0x3171, 0,    0,    0,    0,    0,     0   },
 { NL, "Brug TV"                       , 0x3172, 0,    0,    0,    0,    0,     0   },
 { NL, "TV Limburg"                    , 0x3173, 0,    0,    0,    0,    0,     0   },
 { DE, "RTL Television"                , 0x31C0, 0,    0,    0,    0,    0xDAB, 0   }, /* hmmm..., 31C0?? */
 { BE, "VRT TV1"                       , 0x3201, 0x16, 0x1,  0x36, 0x3,  0,     0   },
 { BE, "CANVAS"                        , 0x3202, 0x16, 0x2,  0x36, 0x2,  0,     0   },
 { BE, "RTBF 1"                        , 0x3203, 0,    0,    0,    0,    0,     0   },
 { BE, "RTBF 2"                        , 0x3204, 0,    0,    0,    0,    0,     0   },
 { BE, "VTM"                           , 0x3205, 0x16, 0x5,  0x36, 0x5,  0,     0   },
 { BE, "Kanaal2"                       , 0x3206, 0x16, 0x6,  0x36, 0x6,  0,     0   },
 { BE, "RTBF Sat"                      , 0x3207, 0,    0,    0,    0,    0,     0   },
 { BE, "RTBF"                          , 0x3208, 0,    0,    0,    0,    0,     0   },
 { LU, "RTL-TVI"                       , 0x3209, 0,    0,    0,    0,    0,     0   },
 { LU, "CLUB-RTL"                      , 0x320A, 0,    0,    0,    0,    0,     0   },
 { GB, "National Geographic Channel"   , 0x320B, 0,    0,    0,    0,    0,     0   },
 { BE, "AB3"                           , 0x320C, 0,    0,    0,    0,    0,     0   },
 { BE, "AB4e"                          , 0x320D, 0,    0,    0,    0,    0,     0   },
 { BE, "Ring TV"                       , 0x320E, 0,    0,    0,    0,    0,     0   },
 { BE, "JIM.tv"                        , 0x320F, 0,    0,    0,    0,    0,     0   },
 { BE, "RTV-Kempen"                    , 0x3210, 0,    0,    0,    0,    0,     0   },
 { BE, "RTV-Mechelen"                  , 0x3211, 0,    0,    0,    0,    0,     0   },
 { BE, "MCM Belgium"                   , 0x3212, 0,    0,    0,    0,    0,     0   },
 { BE, "Vitaya"                        , 0x3213, 0,    0,    0,    0,    0,     0   },
 { BE, "WTV"                           , 0x3214, 0,    0,    0,    0,    0,     0   },
 { BE, "FocusTV"                       , 0x3215, 0,    0,    0,    0,    0,     0   },
 { BE, "Be 1 ana"                      , 0x3216, 0,    0,    0,    0,    0,     0   },
 { BE, "Be 1 num"                      , 0x3217, 0,    0,    0,    0,    0,     0   },
 { BE, "Be Cin� 1"                     , 0x3218, 0,    0,    0,    0,    0,     0   },
 { BE, "Be Sport 1"                    , 0x3219, 0,    0,    0,    0,    0,     0   },
 { BE, "PRIME Sport 1"                 , 0x321A, 0,    0,    0,    0,    0,     0   },
 { BE, "PRIME SPORT 2"                 , 0x321B, 0,    0,    0,    0,    0,     0   },
 { BE, "PRIME Action"                  , 0x321C, 0,    0,    0,    0,    0,     0   },
 { BE, "PRIME One"                     , 0x321D, 0,    0,    0,    0,    0,     0   },
 { BE, "TV Brussel"                    , 0x321E, 0,    0,    0,    0,    0,     0   },
 { BE, "AVSe"                          , 0x321F, 0,    0,    0,    0,    0,     0   },
 { BE, "S televisie"                   , 0x3220, 0,    0,    0,    0,    0,     0   },
 { BE, "TV Limburg"                    , 0x3221, 0,    0,    0,    0,    0,     0   },
 { BE, "Kanaal 3"                      , 0x3222, 0,    0,    0,    0,    0,     0   },
 { BE, "ATV"                           , 0x3223, 0,    0,    0,    0,    0,     0   },
 { BE, "ROB TV"                        , 0x3224, 0,    0,    0,    0,    0,     0   },
 { LU, "PLUG TV"                       , 0x3225, 0,    0,    0,    0,    0,     0   },
 { BE, "Sporza"                        , 0x3226, 0,    0,    0,    0,    0,     0   },
 { BE, "VIJF tv"                       , 0x3227, 0,    0,    0,    0,    0,     0   },
 { BE, "Life!tv"                       , 0x3228, 0,    0,    0,    0,    0,     0   },
 { BE, "MTV Belgium (French)"          , 0x3229, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Sport NL"                 , 0x322A, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Culture NL"               , 0x322B, 0,    0,    0,    0,    0,     0   },
 { BE, "Acht"                          , 0x322C, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Sport FR"                 , 0x322D, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Culture FR"               , 0x322E, 0,    0,    0,    0,    0,     0   },
 { BE, "Discovery Flanders"            , 0x322F, 0,    0,    0,    0,    0,     0   },
 { BE, "T�l� Bruxelles"                , 0x3230, 0,    0,    0,    0,    0,     0   },
 { BE, "T�l�sambre"                    , 0x3231, 0,    0,    0,    0,    0,     0   },
 { BE, "TV Com"                        , 0x3232, 0,    0,    0,    0,    0,     0   },
 { BE, "Canal Zoom"                    , 0x3233, 0,    0,    0,    0,    0,     0   },
 { BE, "Vid�oscope"                    , 0x3234, 0,    0,    0,    0,    0,     0   },
 { BE, "Canal C"                       , 0x3235, 0,    0,    0,    0,    0,     0   },
 { BE, "T�l� MB"                       , 0x3236, 0,    0,    0,    0,    0,     0   },
 { BE, "Antenne Centre"                , 0x3237, 0,    0,    0,    0,    0,     0   },
 { BE, "T�l�vesdre"                    , 0x3238, 0,    0,    0,    0,    0,     0   },
 { BE, "RTC T�l� Li�ge"                , 0x3239, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Plus NL"                  , 0x323A, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Plus FR"                  , 0x323B, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Life NL"                  , 0x323C, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI Life FR"                  , 0x323D, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI News NL"                  , 0x323E, 0,    0,    0,    0,    0,     0   },
 { BE, "EXQI News FR"                  , 0x323F, 0,    0,    0,    0,    0,     0   },
 { BE, "No tele"                       , 0x3240, 0,    0,    0,    0,    0,     0   },
 { BE, "TV Lux"                        , 0x3241, 0,    0,    0,    0,    0,     0   },
 { BE, "Radio Contact Vision"          , 0x3242, 0,    0,    0,    0,    0,     0   },
 { BE, "Kanaal Z - NL"                 , 0x325A, 0,    0,    0,    0,    0,     0   },
 { BE, "CANAL Z - FR"                  , 0x325B, 0,    0,    0,    0,    0,     0   },
 { BE, "CARTOON Network - NL"          , 0x326A, 0,    0,    0,    0,    0,     0   },
 { BE, "CARTOON Network - FR"          , 0x326B, 0,    0,    0,    0,    0,     0   },
 { BE, "LIBERTY CHANNEL - NL"          , 0x327A, 0,    0,    0,    0,    0,     0   },
 { BE, "LIBERTY CHANNEL - FR"          , 0x327B, 0,    0,    0,    0,    0,     0   },
 { BE, "TCM - NL"                      , 0x328A, 0,    0,    0,    0,    0,     0   },
 { BE, "TCM - FR"                      , 0x328B, 0,    0,    0,    0,    0,     0   },
 { BE, "Mozaiek/Mosaique"              , 0x3298, 0,    0,    0,    0,    0,     0   },
 { BE, "Info Kanaal/Canal Info"        , 0x3299, 0,    0,    0,    0,    0,     0   },
 { BE, "Be 1 + 1h"                     , 0x32A7, 0,    0,    0,    0,    0,     0   },
 { BE, "Be Cin� 2"                     , 0x32A8, 0,    0,    0,    0,    0,     0   },
 { BE, "Be Sport 2"                    , 0x32A9, 0,    0,    0,    0,    0,     0   },
 { FR, "Arte / La Cinqui�me"           , 0x330A, 0x2F, 0xA,  0x3F, 0xA,  0,     0   },
 { FR, "RFO1"                          , 0x3311, 0x2F, 0x11, 0x3F, 0x11, 0,     0   },
 { FR, "RFO2"                          , 0x3312, 0x2F, 0x12, 0x3F, 0x12, 0,     0   },
 { FR, "Aqui TV"                       , 0x3320, 0x2F, 0x20, 0x3F, 0x20, 0,     0   },
 { FR, "TLM"                           , 0x3321, 0x2F, 0x21, 0x3F, 0x21, 0,     0   },
 { FR, "TLT"                           , 0x3322, 0x2F, 0x22, 0x3F, 0x22, 0,     0   },
 { IR, "TV3"                           , 0x3333, 0,    0,    0,    0,    0,     0   },
 { FR, "Sailing Channel"               , 0x33B2, 0,    0,    0,    0,    0,     0   },
 { FR, "AB1"                           , 0x33C1, 0x2F, 0xC1, 0x3F, 0x41, 0,     0   },
 { FR, "Canal J"                       , 0x33C2, 0x2F, 0xC2, 0x3F, 0x42, 0,     0   },
 { FR, "Canal Jimmy"                   , 0x33C3, 0x2F, 0xC3, 0x3F, 0x43, 0,     0   },
 { FR, "LCI"                           , 0x33C4, 0x2F, 0xC4, 0x3F, 0x44, 0,     0   },
 { FR, "La Cha�ne M�t�o"               , 0x33C5, 0x2F, 0xC5, 0x3F, 0x45, 0,     0   },
 { FR, "MCM"                           , 0x33C6, 0x2F, 0xC6, 0x3F, 0x46, 0,     0   },
 { FR, "TMC Monte-Carlo"               , 0x33C7, 0x2F, 0xC7, 0x3F, 0x47, 0,     0   },
 { FR, "Paris Premi�re"                , 0x33C8, 0x2F, 0xC8, 0x3F, 0x48, 0,     0   },
 { FR, "Plan�te"                       , 0x33C9, 0x2F, 0xC9, 0x3F, 0x49, 0,     0   },
 { FR, "S�rie Club"                    , 0x33CA, 0x2F, 0xCA, 0x3F, 0x4A, 0,     0   },
 { FR, "T�l�toon"                      , 0x33CB, 0x2F, 0xCB, 0x3F, 0x4B, 0,     0   },
 { FR, "T�va"                          , 0x33CC, 0x2F, 0xCC, 0x3F, 0x4C, 0,     0   },
 { FR, "TF1"                           , 0x33F1, 0x2F, 0x1,  0x3F, 0x1,  0,     0   },
 { FR, "France 2"                      , 0x33F2, 0x2F, 0x2,  0x3F, 0x2,  0,     0   },
 { FR, "France 3"                      , 0x33F3, 0x2F, 0x3,  0x3F, 0x3,  0,     0   },
 { FR, "Canal+"                        , 0x33F4, 0x2F, 0x4,  0x3F, 0x4,  0,     0   },
 { FR, "M6"                            , 0x33F6, 0x2F, 0x6,  0x3F, 0x6,  0,     0   },
 { ES, "ETB 2"                         , 0x3402, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9"                       , 0x3403, 0,    0,    0,    0,    0,     0   },
 { ES, "PUNT 2"                        , 0x3404, 0,    0,    0,    0,    0,     0   },
 { ES, "CCV"                           , 0x3405, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 NEWS 24H"              , 0x3406, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9"                       , 0x3407, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3408, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3409, 0,    0,    0,    0,    0,     0   },
 { ES, "Arte"                          , 0x340A, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x340B, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x340C, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x340D, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x340E, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x340F, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3410, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3411, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3412, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3413, 0,    0,    0,    0,    0,     0   },
 { ES, "CANAL 9 DVB"                   , 0x3414, 0,    0,    0,    0,    0,     0   },
 { ES, "Canal Extremadura TV"          , 0x3415, 0,    0,    0,    0,    0,     0   },
 { ES, "Extremadura TV"                , 0x3416, 0,    0,    0,    0,    0,     0   },
 { ES, "Telemadrid"                    , 0x3420, 0x3E, 0x20, 0x3E, 0x20, 0,     0   },
 { ES, "La Otra"                       , 0x3421, 0x3E, 0x21, 0x3E, 0x21, 0,     0   },
 { ES, "TM SAT"                        , 0x3422, 0x3E, 0x22, 0x3E, 0x22, 0,     0   },
 { ES, "La sexta"                      , 0x3423, 0x3E, 0x23, 0x3E, 0x23, 0,     0   },
 { ES, "Antena 3"                      , 0x3424, 0x3E, 0x24, 0x3E, 0x24, 0,     0   },
 { ES, "Neox"                          , 0x3425, 0x3E, 0x25, 0x3E, 0x25, 0,     0   },
 { ES, "Nova"                          , 0x3426, 0x3E, 0x26, 0x3E, 0x26, 0,     0   },
 { ES, "Cuatro"                        , 0x3427, 0x3E, 0x27, 0x3E, 0x27, 0,     0   },
 { ES, "CNN+"                          , 0x3428, 0x3E, 0x28, 0x3E, 0x28, 0,     0   },
 { ES, "40 Latino"                     , 0x3429, 0x3E, 0x29, 0x3E, 0x29, 0,     0   },
 { ES, "24 Horas"                      , 0x342A, 0x3E, 0x2A, 0x3E, 0x2A, 0,     0   },
 { ES, "Clan TVE"                      , 0x342B, 0x3E, 0x2B, 0x3E, 0x2B, 0,     0   },
 { ES, "Teledeporte"                   , 0x342C, 0x3E, 0x2C, 0x3E, 0x2C, 0,     0   },
 { ES, "CyL7"                          , 0x342D, 0,    0,    0,    0,    0,     0   },
 { ES, "8TV"                           , 0x342E, 0,    0,    0,    0,    0,     0   },
 { ES, "EDC2"                          , 0x342F, 0,    0,    0,    0,    0,     0   },
 { ES, "EDC3"                          , 0x3430, 0,    0,    0,    0,    0,     0   },
 { ES, "105tv"                         , 0x3431, 0,    0,    0,    0,    0,     0   },
 { ES, "CyL8"                          , 0x3432, 0,    0,    0,    0,    0,     0   },
 { PT, "RTP1"                          , 0x3510, 0,    0,    0,    0,    0,     0   },
 { PT, "RTP2"                          , 0x3511, 0,    0,    0,    0,    0,     0   },
 { PT, "RTPAF"                         , 0x3512, 0,    0,    0,    0,    0,     0   },
 { PT, "RTPI"                          , 0x3513, 0,    0,    0,    0,    0,     0   },
 { PT, "RTPAZ"                         , 0x3514, 0,    0,    0,    0,    0,     0   },
 { PT, "RTPM"                          , 0x3515, 0,    0,    0,    0,    0,     0   },
 { PT, "Future use"                    , 0x3516, 0,    0,    0,    0,    0,     0   },
 { PT, "Future use"                    , 0x3517, 0,    0,    0,    0,    0,     0   },
 { PT, "Future use"                    , 0x3518, 0,    0,    0,    0,    0,     0   },
 { PT, "Future use"                    , 0x3519, 0,    0,    0,    0,    0,     0   },
 { IR, "RTE1"                          , 0x3531, 0x42, 0x1,  0x32, 0x1,  0,     0   },
 { IR, "Network 2"                     , 0x3532, 0x42, 0x2,  0x32, 0x2,  0,     0   },
 { IR, "Teilifis na, Gaeilge"          , 0x3533, 0x42, 0x3,  0x32, 0x3,  0,     0   },
 { IR, "RTE"                           , 0x3534, 0x42, 0x4,  0x32, 0x4,  0,     0   },
 { IR, "RTE"                           , 0x3535, 0x42, 0x5,  0x32, 0x5,  0,     0   },
 { IR, "RTE"                           , 0x3536, 0x42, 0x6,  0x32, 0x6,  0,     0   },
 { IR, "RTE"                           , 0x3537, 0x42, 0x7,  0x32, 0x7,  0,     0   },
 { IR, "RTE"                           , 0x3538, 0x42, 0x8,  0x32, 0x8,  0,     0   },
 { IR, "RTE"                           , 0x3539, 0x42, 0x9,  0x32, 0x9,  0,     0   },
 { IR, "RTE"                           , 0x353A, 0x42, 0xA,  0x32, 0xA,  0,     0   },
 { IR, "RTE"                           , 0x353B, 0x42, 0xB,  0x32, 0xB,  0,     0   },
 { IR, "RTE"                           , 0x353C, 0x42, 0xC,  0x32, 0xC,  0,     0   },
 { IR, "RTE"                           , 0x353D, 0x42, 0xD,  0x32, 0xD,  0,     0   },
 { IR, "RTE"                           , 0x353E, 0x42, 0xE,  0x32, 0xE,  0,     0   },
 { IR, "RTE"                           , 0x353F, 0x42, 0xF,  0x32, 0xF,  0,     0   },
 { IS, "Rikisutvarpid-Sjonvarp"        , 0x3541, 0,    0,    0,    0,    0,     0   },
 { FI, "YLE1"                          , 0x3581, 0x26, 0x1,  0x36, 0x1,  0,     0   },
 { FI, "YLE2"                          , 0x3582, 0x26, 0x2,  0x36, 0x7,  0,     0   },
 { FI, "YLE"                           , 0x3583, 0x26, 0x3,  0x36, 0x8,  0,     0   },
 { FI, "YLE"                           , 0x3584, 0x26, 0x4,  0x36, 0x9,  0,     0   },
 { FI, "YLE"                           , 0x3585, 0x26, 0x5,  0x36, 0xA,  0,     0   },
 { FI, "YLE"                           , 0x3586, 0x26, 0x6,  0x36, 0xB,  0,     0   },
 { FI, "YLE"                           , 0x3587, 0x26, 0x7,  0x36, 0xC,  0,     0   },
 { FI, "YLE"                           , 0x3588, 0x26, 0x8,  0x36, 0xD,  0,     0   },
 { FI, "YLE"                           , 0x3589, 0x26, 0x9,  0x36, 0xE,  0,     0   },
 { FI, "YLE"                           , 0x358A, 0x26, 0xA,  0x36, 0xF,  0,     0   },
 { FI, "YLE"                           , 0x358B, 0x26, 0xB,  0x36, 0x10, 0,     0   },
 { FI, "YLE"                           , 0x358C, 0x26, 0xC,  0x36, 0x11, 0,     0   },
 { FI, "YLE"                           , 0x358D, 0x26, 0xD,  0x36, 0x12, 0,     0   },
 { FI, "YLE"                           , 0x358E, 0x26, 0xE,  0x36, 0x13, 0,     0   },
 { FI, "OWL3"                          , 0x358F, 0x26, 0xF,  0x36, 0x14, 0,     0   },
 { HU, "MTV1"                          , 0x3601, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV2"                          , 0x3602, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional Budapest"        , 0x3611, 0,    0,    0,    0,    0,     0   },
 { HU, "tv2"                           , 0x3620, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional P�cs"            , 0x3621, 0,    0,    0,    0,    0,     0   },
 { HU, "tv2"                           , 0x3622, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional Szeged"          , 0x3631, 0,    0,    0,    0,    0,     0   },
 { HU, "Duna Televizio"                , 0x3636, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional Szombathely"     , 0x3641, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional Debrecen"        , 0x3651, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1 regional Miskolc"         , 0x3661, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV1"                          , 0x3681, 0,    0,    0,    0,    0,     0   },
 { HU, "MTV2"                          , 0x3682, 0,    0,    0,    0,    0,     0   },
 { GB, "SSVC"                          , 0x37E5, 0x2C, 0x25, 0x3C, 0x25, 0,     0   },
 { IT, "RAI 1"                         , 0x3901, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI 2"                         , 0x3902, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI 3"                         , 0x3903, 0,    0,    0,    0,    0,     0   },
 { IT, "Rete A"                        , 0x3904, 0,    0,    0,    0,    0,     0   },
 { IT, "Canale Italia"                 , 0x3905, 0x15, 0x5,  0,    0,    0,     0   },
 { IT, "7 Gold - Telepadova"           , 0x3906, 0,    0,    0,    0,    0,     0   },
 { IT, "Teleregione"                   , 0x3907, 0,    0,    0,    0,    0,     0   },
 { IT, "Telegenova"                    , 0x3908, 0,    0,    0,    0,    0,     0   },
 { IT, "Telenova"                      , 0x3909, 0,    0,    0,    0,    0,     0   },
 { IT, "Arte"                          , 0x390A, 0,    0,    0,    0,    0,     0   },
 { IT, "Canale Dieci"                  , 0x390B, 0,    0,    0,    0,    0,     0   },
 { IT, "TRS TV"                        , 0x3910, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema Classic"            , 0x3911, 0x15, 0x11, 0,    0,    0,     0   },
 { IT, "Sky Future use (canale 109)"   , 0x3912, 0x15, 0x12, 0,    0,    0,     0   },
 { IT, "Sky Calcio 1"                  , 0x3913, 0x15, 0x13, 0,    0,    0,     0   },
 { IT, "Sky Calcio 2"                  , 0x3914, 0x15, 0x14, 0,    0,    0,     0   },
 { IT, "Sky Calcio 3"                  , 0x3915, 0x15, 0x15, 0,    0,    0,     0   },
 { IT, "Sky Calcio 4"                  , 0x3916, 0x15, 0x16, 0,    0,    0,     0   },
 { IT, "Sky Calcio 5"                  , 0x3917, 0x15, 0x17, 0,    0,    0,     0   },
 { IT, "Sky Calcio 6"                  , 0x3918, 0x15, 0x18, 0,    0,    0,     0   },
 { IT, "Sky Calcio 7"                  , 0x3919, 0x15, 0x19, 0,    0,    0,     0   },
 { IT, "TN8 Telenorba"                 , 0x391A, 0,    0,    0,    0,    0,     0   },
 { IT, "RaiNotizie24"                  , 0x3920, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI Med"                       , 0x3921, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI Sport Pi�"                 , 0x3922, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI Edu1"                      , 0x3923, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI Edu2"                      , 0x3924, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI NettunoSat1"               , 0x3925, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI NettunoSat2"               , 0x3926, 0,    0,    0,    0,    0,     0   },
 { IT, "CameraDeputati"                , 0x3927, 0,    0,    0,    0,    0,     0   },
 { IT, "Senato"                        , 0x3928, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI 4"                         , 0x3929, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI Gulp"                      , 0x392A, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x392B, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x392C, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x392D, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x392E, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x392F, 0,    0,    0,    0,    0,     0   },
 { IT, "Discovery Italy"               , 0x3930, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV VH1"                       , 0x3931, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV Italia"                    , 0x3933, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV Brand New"                 , 0x3934, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV Hits"                      , 0x3935, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV GOLD"                      , 0x3936, 0,    0,    0,    0,    0,     0   },
 { IT, "MTV PULSE"                     , 0x3937, 0,    0,    0,    0,    0,     0   },
 { IT, "RTV38"                         , 0x3938, 0,    0,    0,    0,    0,     0   },
 { IT, "GAY TV"                        , 0x3939, 0,    0,    0,    0,    0,     0   },
 { IT, "TP9 Telepuglia"                , 0x393A, 0,    0,    0,    0,    0,     0   },
 { IT, "Video Italia"                  , 0x3940, 0,    0,    0,    0,    0,     0   },
 { IT, "SAT 2000"                      , 0x3941, 0,    0,    0,    0,    0,     0   },
 { IT, "Jimmy"                         , 0x3942, 0x15, 0x42, 0,    0,    0,     0   },
 { IT, "Planet"                        , 0x3943, 0x15, 0x43, 0,    0,    0,     0   },
 { IT, "Cartoon Network"               , 0x3944, 0x15, 0x44, 0,    0,    0,     0   },
 { IT, "Boomerang"                     , 0x3945, 0x15, 0x45, 0,    0,    0,     0   },
 { IT, "CNN International"             , 0x3946, 0x15, 0x46, 0,    0,    0,     0   },
 { IT, "Cartoon Network +1"            , 0x3947, 0x15, 0x47, 0,    0,    0,     0   },
 { IT, "Sky Sports 3"                  , 0x3948, 0x15, 0x48, 0,    0,    0,     0   },
 { IT, "Sky Diretta Gol"               , 0x3949, 0x15, 0x49, 0,    0,    0,     0   },
 { IT, "TG NORBA"                      , 0x394A, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x3950, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x3951, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat Cinema"                 , 0x3952, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x3953, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat Gambero Rosso"          , 0x3954, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat YoYo"                   , 0x3955, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat Smash"                  , 0x3956, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x3957, 0,    0,    0,    0,    0,     0   },
 { IT, "RAI"                           , 0x3958, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat Extra"                  , 0x3959, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat Premium"                , 0x395A, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat"                        , 0x395B, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat"                        , 0x395C, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat"                        , 0x395D, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat"                        , 0x395E, 0,    0,    0,    0,    0,     0   },
 { IT, "RAISat"                        , 0x395F, 0,    0,    0,    0,    0,     0   },
 { IT, "SCI FI CHANNEL"                , 0x3960, 0x15, 0x60, 0,    0,    0,     0   },
 { IT, "Discovery Civilisations"       , 0x3961, 0,    0,    0,    0,    0,     0   },
 { IT, "Discovery Travel and Adventure", 0x3962, 0,    0,    0,    0,    0,     0   },
 { IT, "Discovery Science"             , 0x3963, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Meteo24"                   , 0x3968, 0x15, 0x68, 0,    0,    0,     0   },
 { IT, "Sky Cinema 2"                  , 0x3970, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema 3"                  , 0x3971, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema Autore"             , 0x3972, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema Max"                , 0x3973, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema 16:9"               , 0x3974, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Sports 2"                  , 0x3975, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky TG24"                      , 0x3976, 0,    0,    0,    0,    0,     0   },
 { IT, "Fox"                           , 0x3977, 0x15, 0x77, 0,    0,    0,     0   },
 { IT, "Foxlife"                       , 0x3978, 0x15, 0x78, 0,    0,    0,     0   },
 { IT, "National Geographic Channel"   , 0x3979, 0x15, 0x79, 0,    0,    0,     0   },
 { IT, "A1"                            , 0x3980, 0x15, 0x80, 0,    0,    0,     0   },
 { IT, "History Channel"               , 0x3981, 0x15, 0x81, 0,    0,    0,     0   },
 { IT, "FOX KIDS"                      , 0x3985, 0,    0,    0,    0,    0,     0   },
 { IT, "PEOPLE TV � RETE 7"            , 0x3986, 0,    0,    0,    0,    0,     0   },
 { IT, "FOX KIDS +1"                   , 0x3987, 0,    0,    0,    0,    0,     0   },
 { IT, "LA7"                           , 0x3988, 0,    0,    0,    0,    0,     0   },
 { IT, "PrimaTV"                       , 0x3989, 0,    0,    0,    0,    0,     0   },
 { IT, "SportItalia"                   , 0x398A, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x398B, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x398C, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x398D, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x398E, 0,    0,    0,    0,    0,     0   },
 { IT, "Espansione TV"                 , 0x398F, 0,    0,    0,    0,    0,     0   },
 { IT, "STUDIO UNIVERSAL"              , 0x3990, 0x15, 0x90, 0,    0,    0,     0   },
 { IT, "Marcopolo"                     , 0x3991, 0x15, 0x91, 0,    0,    0,     0   },
 { IT, "Alice"                         , 0x3992, 0x15, 0x92, 0,    0,    0,     0   },
 { IT, "Nuvolari"                      , 0x3993, 0x15, 0x93, 0,    0,    0,     0   },
 { IT, "Leonardo"                      , 0x3994, 0x15, 0x94, 0,    0,    0,     0   },
 { IT, "SUPERPIPPA CHANNEL"            , 0x3996, 0x15, 0x96, 0,    0,    0,     0   },
 { IT, "Sky Sports 1"                  , 0x3997, 0,    0,    0,    0,    0,     0   },
 { IT, "Sky Cinema 1"                  , 0x3998, 0,    0,    0,    0,    0,     0   },
 { IT, "Tele+3"                        , 0x3999, 0,    0,    0,    0,    0,     0   },
 { IT, "FacileTV"                      , 0x399A, 0x15, 0xA,  0,    0,    0,     0   },
 { IT, "Sitcom 2"                      , 0x399B, 0x15, 0xB,  0,    0,    0,     0   },
 { IT, "Sitcom 3"                      , 0x399C, 0x15, 0xC,  0,    0,    0,     0   },
 { IT, "Sitcom 4"                      , 0x399D, 0x15, 0xD,  0,    0,    0,     0   },
 { IT, "Sitcom 5"                      , 0x399E, 0x15, 0xE,  0,    0,    0,     0   },
 { IT, "Italiani nel Mondo"            , 0x399F, 0x15, 0xF,  0,    0,    0,     0   },
 { IT, "Sky Calcio 8"                  , 0x39A0, 0x15, 0xA0, 0,    0,    0,     0   },
 { IT, "Sky Calcio 9"                  , 0x39A1, 0x15, 0xA1, 0,    0,    0,     0   },
 { IT, "Sky Calcio 10"                 , 0x39A2, 0x15, 0xA2, 0,    0,    0,     0   },
 { IT, "Sky Calcio 11"                 , 0x39A3, 0x15, 0xA3, 0,    0,    0,     0   },
 { IT, "Sky Calcio 12"                 , 0x39A4, 0x15, 0xA4, 0,    0,    0,     0   },
 { IT, "Sky Calcio 13"                 , 0x39A5, 0x15, 0xA5, 0,    0,    0,     0   },
 { IT, "Sky Calcio 14"                 , 0x39A6, 0x15, 0xA6, 0,    0,    0,     0   },
 { IT, "Telesanterno"                  , 0x39A7, 0x15, 0xA7, 0,    0,    0,     0   },
 { IT, "Telecentro"                    , 0x39A8, 0x15, 0xA8, 0,    0,    0,     0   },
 { IT, "Telestense"                    , 0x39A9, 0x15, 0xA9, 0,    0,    0,     0   },
 { IT, "TCS - Telecostasmeralda"       , 0x39AB, 0,    0,    0,    0,    0,     0   },
 { IT, "Disney Channel +1"             , 0x39B0, 0x15, 0xB0, 0,    0,    0,     0   },
 { IT, "Sailing Channel"               , 0x39B1, 0,    0,    0,    0,    0,     0   },
 { IT, "Disney Channel"                , 0x39B2, 0x15, 0xB2, 0,    0,    0,     0   },
 { IT, "7 Gold-Sestra Rete"            , 0x39B3, 0x15, 0xB3, 0,    0,    0,     0   },
 { IT, "Rete 8-VGA"                    , 0x39B4, 0x15, 0xB4, 0,    0,    0,     0   },
 { IT, "Nuovarete"                     , 0x39B5, 0x15, 0xB5, 0,    0,    0,     0   },
 { IT, "Radio Italia TV"               , 0x39B6, 0x15, 0xB6, 0,    0,    0,     0   },
 { IT, "Rete 7"                        , 0x39B7, 0x15, 0xB7, 0,    0,    0,     0   },
 { IT, "E! Entertainment Television"   , 0x39B8, 0x15, 0xB8, 0,    0,    0,     0   },
 { IT, "Toon Disney"                   , 0x39B9, 0x15, 0xB9, 0,    0,    0,     0   },
 { IT, "Play TV Italia"                , 0x39BA, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� A"                , 0x39C1, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� B"                , 0x39C2, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� C"                , 0x39C3, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� D"                , 0x39C4, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� E"                , 0x39C5, 0,    0,    0,    0,    0,     0   },
 { IT, "La7 Cartapi� X"                , 0x39C6, 0,    0,    0,    0,    0,     0   },
 { IT, "Bassano TV"                    , 0x39C7, 0x15, 0xC7, 0,    0,    0,     0   },
 { IT, "ESPN Classic Sport"            , 0x39C8, 0x15, 0xC8, 0,    0,    0,     0   },
 { IT, "VIDEOLINA"                     , 0x39CA, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Premium 5"            , 0x39D1, 0x15, 0xD1, 0,    0,    0,     0   },
 { IT, "Mediaset Premium 1"            , 0x39D2, 0x15, 0xD2, 0,    0,    0,     0   },
 { IT, "Mediaset Premium 2"            , 0x39D3, 0x15, 0xD3, 0,    0,    0,     0   },
 { IT, "Mediaset Premium 3"            , 0x39D4, 0x15, 0xD4, 0,    0,    0,     0   },
 { IT, "Mediaset Premium 4"            , 0x39D5, 0x15, 0xD5, 0,    0,    0,     0   },
 { IT, "BOING"                         , 0x39D6, 0x15, 0xD6, 0,    0,    0,     0   },
 { IT, "Playlist Italia"               , 0x39D7, 0x15, 0xD7, 0,    0,    0,     0   },
 { IT, "MATCH MUSIC"                   , 0x39D8, 0x15, 0xD8, 0,    0,    0,     0   },
 { IT, "Televisiva SUPER3"             , 0x39D9, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediashopping"                 , 0x39DA, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Premium 6"            , 0x39DB, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Premium 7"            , 0x39DC, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x39DD, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset Future Use"           , 0x39DE, 0,    0,    0,    0,    0,     0   },
 { IT, "Iris"                          , 0x39DF, 0,    0,    0,    0,    0,     0   },
 { IT, "National Geographic +1"        , 0x39E1, 0x15, 0xE1, 0,    0,    0,     0   },
 { IT, "History Channel +1"            , 0x39E2, 0x15, 0xE2, 0,    0,    0,     0   },
 { IT, "Sky TV"                        , 0x39E3, 0x15, 0xE3, 0,    0,    0,     0   },
 { IT, "GXT"                           , 0x39E4, 0x15, 0xE4, 0,    0,    0,     0   },
 { IT, "Playhouse Disney"              , 0x39E5, 0x15, 0xE5, 0,    0,    0,     0   },
 { IT, "Sky Canale 224"                , 0x39E6, 0x15, 0xE6, 0,    0,    0,     0   },
 { IT, "Music Box"                     , 0x39E7, 0,    0,    0,    0,    0,     0   },
 { IT, "Tele Liguria Sud"              , 0x39E8, 0,    0,    0,    0,    0,     0   },
 { IT, "TN7 Telenorba"                 , 0x39E9, 0,    0,    0,    0,    0,     0   },
 { IT, "Brescia Punto TV"              , 0x39EA, 0,    0,    0,    0,    0,     0   },
 { IT, "QOOB"                          , 0x39EB, 0,    0,    0,    0,    0,     0   },
 { IT, "AB Channel"                    , 0x39EE, 0,    0,    0,    0,    0,     0   },
 { IT, "Teleradiocity"                 , 0x39F1, 0,    0,    0,    0,    0,     0   },
 { IT, "Teleradiocity Genova"          , 0x39F2, 0,    0,    0,    0,    0,     0   },
 { IT, "Teleradiocity Lombardia"       , 0x39F3, 0,    0,    0,    0,    0,     0   },
 { IT, "Telestar Piemonte"             , 0x39F4, 0,    0,    0,    0,    0,     0   },
 { IT, "Telestar Liguria"              , 0x39F5, 0,    0,    0,    0,    0,     0   },
 { IT, "Telestar Lombardia"            , 0x39F6, 0,    0,    0,    0,    0,     0   },
 { IT, "Italia 8 Piemonte"             , 0x39F7, 0,    0,    0,    0,    0,     0   },
 { IT, "Italia 8 Lombardia"            , 0x39F8, 0,    0,    0,    0,    0,     0   },
 { IT, "Radio Tele Europa"             , 0x39F9, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FA, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FB, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FC, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FD, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FE, 0,    0,    0,    0,    0,     0   },
 { IT, "Mediaset"                      , 0x39FF, 0,    0,    0,    0,    0,     0   },
 { ES, "TVE1"                          , 0x3E00, 0,    0,    0,    0,    0,     0   },
 { LU, "RTL T�l� L�tzebuerg"           , 0x4000, 0,    0,    0,    0,    0x791, 101 },
 { LU, "RTL TVI 20 ANS"                , 0x4020, 0,    0,    0,    0,    0,     0   },
 { CH, "SF 1"                          , 0x4101, 0x24, 0xC1, 0x34, 0x41, 0x4C1, 101 },
 { CH, "TSR 1"                         , 0x4102, 0x24, 0xC2, 0x34, 0x42, 0x4C2, 102 },
 { CH, "TSI 1"                         , 0x4103, 0x24, 0xC3, 0x34, 0x43, 0x4C3, 103 },
 { CH, "SF 2"                          , 0x4107, 0x24, 0xC7, 0x34, 0x47, 0x4C7, 107 },
 { CH, "TSR 2"                         , 0x4108, 0x24, 0xC8, 0x34, 0x48, 0x4C8, 108 },
 { CH, "TSI 2"                         , 0x4109, 0x24, 0xC9, 0x34, 0x49, 0x4C9, 109 },
 { CH, "SAT ACCESS"                    , 0x410A, 0x24, 0xCA, 0x34, 0x4A, 0x4CA, 110 },
 { CH, "SFi"                           , 0,      0x24, 0,    0,    0,    0x4CC, 112 },
 { CH, "TSR info"                      , 0,      0x24, 0,    0,    0,    0x4CD, 113 },
 { CH, "U1"                            , 0x4121, 0x24, 0x21, 0,    0,    0x495, 221 },
 { CH, "TeleZ�ri"                      , 0x4122, 0x24, 0x22, 0,    0,    0x481, 201 },
 { CH, "Teleclub Abo"                  , 0,      0x24, 0,    0,    0,    0x482, 202 },
 { CH, "TV Z�ri"                       , 0,      0x24, 0,    0,    0,    0x483, 203 },
 { CH, "TeleBern"                      , 0,      0x24, 0,    0,    0,    0x484, 204 },
 { CH, "Tele M1"                       , 0,      0x24, 0,    0,    0,    0x485, 205 },
 { CH, "Star TV"                       , 0,      0x24, 0,    0,    0,    0x486, 206 },
 { CH, "ProSieben"                     , 0,      0x24, 0,    0,    0,    0x487, 207 },
 { CH, "TopTV"                         , 0,      0x24, 0,    0,    0,    0x488, 208 },
 { CH, "Tele 24"                       , 0,      0x24, 0,    0,    0,    0x489, 209 },
 { CH, "kabel eins"                    , 0,      0x24, 0,    0,    0,    0x48A, 210 },
 { CH, "TV3"                           , 0,      0x24, 0,    0,    0,    0x48B, 211 },
 { CH, "TeleZ�ri 2"                    , 0,      0x24, 0,    0,    0,    0x48C, 212 },
 { CH, "Swizz Music Television"        , 0,      0x24, 0,    0,    0,    0x48D, 213 },
 { CH, "Intro TV"                      , 0,      0x24, 0,    0,    0,    0x48E, 214 },
 { CH, "Tele Tell"                     , 0,      0x24, 0,    0,    0,    0x48F, 215 },
 { CH, "Tele Top"                      , 0,      0x24, 0,    0,    0,    0x490, 216 },
 { CH, "TSO CH"                        , 0,      0x24, 0,    0,    0,    0x491, 217 },
 { CH, "TVO"                           , 0,      0x24, 0,    0,    0,    0x492, 218 },
 { CH, "Tele TI"                       , 0,      0x24, 0,    0,    0,    0x493, 219 },
 { CH, "SHf"                           , 0,      0x24, 0,    0,    0,    0x494, 220 },
 { CH, "MTV Swiss"                     , 0,      0x24, 0,    0,    0,    0x496, 222 },
 { CH, "3+"                            , 0,      0x24, 0,    0,    0,    0x497, 223 },
 { CH, "telebasel"                     , 0,      0x24, 0,    0,    0,    0x498, 224 },
 { CH, "NICK Swiss"                    , 0,      0x24, 0,    0,    0,    0x499, 225 },
 { CH, "SSF"                           , 0,      0x24, 0,    0,    0,    0x49A, 226 },
 { CH, "TELE-1"                        , 0,      0x24, 0,    0,    0,    0x49B, 227 },
 { CZ, "Barrandov TV"                  , 0x4200, 0x32, 0,    0,    0,    0,     0   },
 { CZ, "CT 1"                          , 0x4201, 0x32, 0xC1, 0x3C, 0x21, 0,     0   },
 { CZ, "CT 2"                          , 0x4202, 0x32, 0xC2, 0x3C, 0x22, 0,     0   },
 { CZ, "NOVA TV"                       , 0x4203, 0x32, 0xC3, 0x3C, 0x23, 0,     0   },
 { CZ, "Prima TV"                      , 0x4204, 0x32, 0xC4, 0x3C, 0x4,  0,     0   },
 { CZ, "TV Praha"                      , 0x4205, 0x32, 0,    0,    0,    0,     0   },
 { CZ, "TV HK"                         , 0x4206, 0x32, 0,    0,    0,    0,     0   },
 { CZ, "TV Pardubice"                  , 0x4207, 0x32, 0,    0,    0,    0,     0   },
 { CZ, "TV Brno"                       , 0x4208, 0x32, 0,    0,    0,    0,     0   },
 { CZ, "Prima COOL"                    , 0x4209, 0x32, 0xC9, 0x3C, 0xA,  0,     0   },
 { CZ, "CT24"                          , 0x420A, 0x32, 0xCA, 0,    0,    0,     0   },
 { CZ, "CT4 SPORT"                     , 0x420B, 0x32, 0xCB, 0,    0,    0,     0   },
 { CZ, "CT"                            , 0x420C, 0x32, 0xCC, 0,    0,    0,     0   },
 { CZ, "CT"                            , 0x420D, 0x32, 0xCD, 0,    0,    0,     0   },
 { CZ, "CT"                            , 0x420E, 0x32, 0xCE, 0,    0,    0,     0   },
 { CZ, "CT"                            , 0x420F, 0x32, 0xCF, 0,    0,    0,     0   },
 { CZ, "Ocko TV"                       , 0x4210, 0x32, 0x10, 0x3B, 0x10, 0,     0   },
 { CZ, "CT1 Regional Brno"             , 0x4211, 0x32, 0xD1, 0x3B, 0x1,  0,     0   },
 { CZ, "CT2 Regional Brno"             , 0x4212, 0x32, 0xD2, 0x3B, 0x4,  0,     0   },
 { CZ, "NOVA CINEMA"                   , 0x4213, 0x32, 0xD3, 0x3B, 0x13, 0,     0   },
 { CZ, "GALAXIE SPORT"                 , 0x4214, 0x32, 0xD4, 0x3B, 0x14, 0,     0   },
 { CZ, "CT1 Regional Ostravia"         , 0x4221, 0x32, 0xE1, 0x3B, 0x2,  0,     0   },
 { CZ, "CT2 Regional Ostravia"         , 0x4222, 0x32, 0xE2, 0x3B, 0x5,  0,     0   },
 { CZ, "CT1 Regional"                  , 0x4231, 0x32, 0xF1, 0x3C, 0x25, 0,     0   },
 { CZ, "CT2 Regional"                  , 0x4232, 0x32, 0xF2, 0x3B, 0x3,  0,     0   },
 { CZ, "Z1 TV"                         , 0x4233, 0x32, 0xF3, 0x3C, 0x3,  0,     0   },
 { SK, "STV1"                          , 0x42A1, 0x35, 0xA1, 0x35, 0x21, 0,     0   },
 { SK, "STV2"                          , 0x42A2, 0x35, 0xA2, 0x35, 0x22, 0,     0   },
 { SK, "STV3"                          , 0x42A3, 0x35, 0xA3, 0x35, 0x23, 0,     0   },
 { SK, "STV4"                          , 0x42A4, 0x35, 0xA4, 0x35, 0x24, 0,     0   },
 { SK, "future use"                    , 0x42A5, 0x35, 0xA5, 0x35, 0x25, 0,     0   },
 { SK, "future use"                    , 0x42A6, 0x35, 0xA6, 0x35, 0x26, 0,     0   },
 { SK, "future use"                    , 0x42A7, 0x35, 0xA7, 0x35, 0x27, 0,     0   },
 { SK, "future use"                    , 0x42A8, 0x35, 0xA8, 0x35, 0x28, 0,     0   },
 { SK, "future use"                    , 0x42A9, 0x35, 0xA9, 0x35, 0x29, 0,     0   },
 { SK, "future use"                    , 0x42AA, 0x35, 0xAA, 0x35, 0x2A, 0,     0   },
 { SK, "future use"                    , 0x42AB, 0x35, 0xAB, 0x35, 0x2B, 0,     0   },
 { SK, "future use"                    , 0x42AC, 0x35, 0xAC, 0x35, 0x2C, 0,     0   },
 { SK, "future use"                    , 0x42AD, 0x35, 0xAD, 0x35, 0x2D, 0,     0   },
 { SK, "future use"                    , 0x42AE, 0x35, 0xAE, 0x35, 0x2E, 0,     0   },
 { SK, "future use"                    , 0x42AF, 0x35, 0xAF, 0x35, 0x2F, 0,     0   },
 { SK, "TV JOJ"                        , 0x42B1, 0x35, 0xB1, 0x35, 0x11, 0,     0   },
 { AT, "ORF-1"                         , 0x4301, 0x1A, 0,    0,    0,    0xAC1, 101 },
 { AT, "ORF-2"                         , 0x4302, 0x1A, 0,    0,    0,    0xAC2, 102 },
 { AT, "ORF-3"                         , 0,      0x1A, 0,    0,    0,    0xAC3, 103 },
 { AT, "ORF-Sport +"                   , 0,      0x1A, 0,    0,    0,    0xAC4, 104 },
 { AT, "TW 1"                          , 0,      0x1A, 0,    0,    0,    0xAC7, 107 },
 { AT, "Nick - Viva"                   , 0,      0x1A, 0,    0,    0,    0xAC8, 108 },
 { AT, "MTV Austria"                   , 0,      0x1A, 0,    0,    0,    0xAC9, 109 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xACB, 111 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xACC, 112 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xACD, 113 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xACE, 114 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xACF, 115 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xAD0, 116 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xAD1, 117 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xAD2, 118 },
 { AT, "ORF-2"                         , 0,      0x1A, 0,    0,    0,    0xAD3, 119 },
 { AT, "ORF"                           , 0x4303, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4304, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4305, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4306, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4307, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4308, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x4309, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x430A, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ORF"                           , 0x430B, 0x1A, 0,    0,    0,    0,     0   },
 { AT, "ATV"                           , 0x430C, 0x1A, 0,    0,    0,    0xACA, 110 },
 { GB, "UK GOLD"                       , 0x4401, 0x5B, 0xFA, 0x3B, 0x7A, 0,     0   },
 { GB, "UK LIVING"                     , 0x4402, 0x2C, 0x1,  0x3C, 0x1,  0,     0   },
 { GB, "Channel 5"                     , 0,      0x2C, 0x2,  0,    0,    0,     0   },
 { GB, "Sianel Pedwar Cymru"           , 0,      0x2C, 0x7,  0,    0,    0,     0   },
 { GB, "Channel 4"                     , 0,      0x2C, 0x11, 0,    0,    0,     0   },
 { GB, "Scottish TV"                   , 0,      0x2C, 0x12, 0,    0,    0,     0   },
 { GB, "Anglia"                        , 0,      0x2C, 0x1C, 0,    0,    0,     0   },
 { GB, "ITV Network"                   , 0,      0x2C, 0x1E, 0,    0,    0,     0   },
 { GB, "WIRE TV"                       , 0x4403, 0x2C, 0x3C, 0x3C, 0x3C, 0,     0   },
 { GB, "CHILDREN'S CHANNEL"            , 0x4404, 0x5B, 0xF0, 0x3B, 0x70, 0,     0   },
 { GB, "BRAVO"                         , 0x4405, 0x5B, 0xEF, 0x3B, 0x6F, 0,     0   },
 { GB, "LEARNING CHANNEL"              , 0x4406, 0x5B, 0xF7, 0x3B, 0x77, 0,     0   },
 { GB, "DISCOVERY"                     , 0x4407, 0x5B, 0xF2, 0x3B, 0x72, 0,     0   },
 { GB, "FAMILY CHANNEL"                , 0x4408, 0x5B, 0xF3, 0x3B, 0x73, 0,     0   },
 { GB, "Live TV"                       , 0x4409, 0x5B, 0xF8, 0x3B, 0x78, 0,     0   },
 { GB, "UK GOLD"                       , 0x4411, 0x5B, 0xFB, 0x3B, 0x7B, 0,     0   },
 { GB, "UK GOLD"                       , 0x4412, 0x5B, 0xFC, 0x3B, 0x7C, 0,     0   },
 { GB, "UK GOLD"                       , 0x4413, 0x5B, 0xFD, 0x3B, 0x7D, 0,     0   },
 { GB, "UK GOLD"                       , 0x4414, 0x5B, 0xFE, 0x3B, 0x7E, 0,     0   },
 { GB, "UK GOLD"                       , 0x4415, 0x5B, 0xFF, 0x3B, 0x7F, 0,     0   },
 { GB, "Discovery Home & Leisure"      , 0x4420, 0,    0,    0,    0,    0,     0   },
 { GB, "Animal Planet"                 , 0x4421, 0,    0,    0,    0,    0,     0   },
 { GB, "BBC2"                          , 0x443E, 0x2C, 0x3E, 0x3C, 0x3E, 0,     0   },
 { GB, "BBC2"                          , 0x4440, 0x2C, 0x40, 0x3C, 0x40, 0,     0   },
 { GB, "BBC1 NI"                       , 0x4441, 0x2C, 0x41, 0x3C, 0x41, 0,     0   },
 { GB, "BBC2 Wales"                    , 0x4442, 0x2C, 0x42, 0x3C, 0x42, 0,     0   },
 { GB, "BBC1"                          , 0x4443, 0x2C, 0x43, 0x3C, 0x43, 0,     0   },
 { GB, "BBC2 Scotland"                 , 0x4444, 0x2C, 0x44, 0x3C, 0x44, 0,     0   },
 { GB, "BBC1"                          , 0x4445, 0x2C, 0x45, 0x3C, 0x45, 0,     0   },
 { GB, "BBC2"                          , 0x4446, 0x2C, 0x46, 0x3C, 0x46, 0,     0   },
 { GB, "BBC1"                          , 0x4447, 0x2C, 0x47, 0x3C, 0x47, 0,     0   },
 { GB, "BBC2"                          , 0x4448, 0x2C, 0x48, 0x3C, 0x48, 0,     0   },
 { GB, "BBC1"                          , 0x4449, 0x2C, 0x49, 0x3C, 0x49, 0,     0   },
 { GB, "BBC2"                          , 0x444A, 0x2C, 0x4A, 0x3C, 0x4A, 0,     0   },
 { GB, "BBC1"                          , 0x444B, 0x2C, 0x4B, 0x3C, 0x4B, 0,     0   },
 { GB, "BBC2"                          , 0x444C, 0x2C, 0x4C, 0x3C, 0x4C, 0,     0   },
 { GB, "BBC1"                          , 0x444D, 0x2C, 0x4D, 0x3C, 0x4D, 0,     0   },
 { GB, "BBC2"                          , 0x444E, 0x2C, 0x4E, 0x3C, 0x4E, 0,     0   },
 { GB, "BBC1"                          , 0x444F, 0x2C, 0x4F, 0x3C, 0x4F, 0,     0   },
 { GB, "BBC2"                          , 0x4450, 0x2C, 0x50, 0x3C, 0x50, 0,     0   },
 { GB, "BBC1"                          , 0x4451, 0x2C, 0x51, 0x3C, 0x51, 0,     0   },
 { GB, "BBC2"                          , 0x4452, 0x2C, 0x52, 0x3C, 0x52, 0,     0   },
 { GB, "BBC1"                          , 0x4453, 0x2C, 0x53, 0x3C, 0x53, 0,     0   },
 { GB, "BBC2"                          , 0x4454, 0x2C, 0x54, 0x3C, 0x54, 0,     0   },
 { GB, "BBC1"                          , 0x4455, 0x2C, 0x55, 0x3C, 0x55, 0,     0   },
 { GB, "BBC2"                          , 0x4456, 0x2C, 0x56, 0x3C, 0x56, 0,     0   },
 { GB, "BBC World"                     , 0x4457, 0x2C, 0x57, 0x3C, 0x57, 0,     0   },
 { GB, "BBC World"                     , 0x4458, 0x2C, 0x58, 0x3C, 0x58, 0,     0   },
 { GB, "BBC World"                     , 0x4459, 0x2C, 0x59, 0x3C, 0x59, 0,     0   },
 { GB, "BBC World"                     , 0x445A, 0x2C, 0x5A, 0x3C, 0x5A, 0,     0   },
 { GB, "BBC World"                     , 0x445B, 0x2C, 0x5B, 0x3C, 0x5B, 0,     0   },
 { GB, "BBC World"                     , 0x445C, 0x2C, 0x5C, 0x3C, 0x5C, 0,     0   },
 { GB, "BBC World"                     , 0x445D, 0x2C, 0x5D, 0x3C, 0x5D, 0,     0   },
 { GB, "BBC World"                     , 0x445E, 0x2C, 0x5E, 0x3C, 0x5E, 0,     0   },
 { GB, "BBC World"                     , 0x445F, 0x2C, 0x5F, 0x3C, 0x5F, 0,     0   },
 { GB, "BBC World"                     , 0x4460, 0x2C, 0x60, 0x3C, 0x60, 0,     0   },
 { GB, "BBC World"                     , 0x4461, 0x2C, 0x61, 0x3C, 0x61, 0,     0   },
 { GB, "BBC World"                     , 0x4462, 0x2C, 0x62, 0x3C, 0x62, 0,     0   },
 { GB, "BBC World"                     , 0x4463, 0x2C, 0x63, 0x3C, 0x63, 0,     0   },
 { GB, "BBC World"                     , 0x4464, 0x2C, 0x64, 0x3C, 0x64, 0,     0   },
 { GB, "BBC World"                     , 0x4465, 0x2C, 0x65, 0x3C, 0x65, 0,     0   },
 { GB, "BBC World"                     , 0x4466, 0x2C, 0x66, 0x3C, 0x66, 0,     0   },
 { GB, "BBC World"                     , 0x4467, 0x2C, 0x67, 0x3C, 0x67, 0,     0   },
 { GB, "BBC Prime"                     , 0x4468, 0x2C, 0x68, 0x3C, 0x68, 0,     0   },
 { GB, "BBC News 24"                   , 0x4469, 0x2C, 0x69, 0x3C, 0x69, 0,     0   },
 { GB, "BBC2"                          , 0x446A, 0x2C, 0x6A, 0x3C, 0x6A, 0,     0   },
 { GB, "BBC1"                          , 0x446B, 0x2C, 0x6B, 0x3C, 0x6B, 0,     0   },
 { GB, "BBC2"                          , 0x446C, 0x2C, 0x6C, 0x3C, 0x6C, 0,     0   },
 { GB, "BBC1"                          , 0x446D, 0x2C, 0x6D, 0x3C, 0x6D, 0,     0   },
 { GB, "BBC2"                          , 0x446E, 0x2C, 0x6E, 0x3C, 0x6E, 0,     0   },
 { GB, "BBC1"                          , 0x446F, 0x2C, 0x6F, 0x3C, 0x6F, 0,     0   },
 { GB, "BBC2"                          , 0x4470, 0x2C, 0x70, 0x3C, 0x70, 0,     0   },
 { GB, "BBC1"                          , 0x4471, 0x2C, 0x71, 0x3C, 0x71, 0,     0   },
 { GB, "BBC2"                          , 0x4472, 0x2C, 0x72, 0x3C, 0x72, 0,     0   },
 { GB, "BBC1"                          , 0x4473, 0x2C, 0x73, 0x3C, 0x73, 0,     0   },
 { GB, "BBC2"                          , 0x4474, 0x2C, 0x74, 0x3C, 0x74, 0,     0   },
 { GB, "BBC1"                          , 0x4475, 0x2C, 0x75, 0x3C, 0x75, 0,     0   },
 { GB, "BBC2"                          , 0x4476, 0x2C, 0x76, 0x3C, 0x76, 0,     0   },
 { GB, "BBC1"                          , 0x4477, 0x2C, 0x77, 0x3C, 0x77, 0,     0   },
 { GB, "BBC2"                          , 0x4478, 0x2C, 0x78, 0x3C, 0x78, 0,     0   },
 { GB, "BBC1"                          , 0x4479, 0x2C, 0x79, 0x3C, 0x79, 0,     0   },
 { GB, "BBC2"                          , 0x447A, 0x2C, 0x7A, 0x3C, 0x7A, 0,     0   },
 { GB, "BBC1 Scotland"                 , 0x447B, 0x2C, 0x7B, 0x3C, 0x7B, 0,     0   },
 { GB, "BBC2 future 01"                , 0x447C, 0x2C, 0x7C, 0x3C, 0x7C, 0,     0   },
 { GB, "BBC1 Wales"                    , 0x447D, 0x2C, 0x7D, 0x3C, 0x7D, 0,     0   },
 { GB, "BBC2 NI"                       , 0x447E, 0x2C, 0x7E, 0x3C, 0x7E, 0,     0   },
 { GB, "BBC1"                          , 0x447F, 0x2C, 0x7F, 0x3C, 0x7F, 0,     0   },
 { GB, "TNT / Cartoon Network"         , 0x44C1, 0,    0,    0,    0,    0,     0   },
 { GB, "DISNEY CHANNEL UK"             , 0x44D1, 0x5B, 0xCC, 0x3B, 0x4C, 0,     0   },
 { DK, "TV 2"                          , 0x4502, 0x29, 0x2,  0x39, 0x2,  0,     0   },
 { DK, "TV 2 Zulu"                     , 0x4503, 0x29, 0x4,  0x39, 0x4,  0,     0   },
 { DK, "Discovery Denmark"             , 0x4504, 0,    0,    0,    0,    0,     0   },
 { DK, "TV 2 Charlie"                  , 0x4505, 0x29, 0x5,  0,    0,    0,     0   },
 { DK, "TV Danmark"                    , 0x4506, 0x29, 0x6,  0,    0,    0,     0   },
 { DK, "Kanal 5"                       , 0x4507, 0x29, 0x7,  0,    0,    0,     0   },
 { DK, "TV 2 Film"                     , 0x4508, 0x29, 0x8,  0,    0,    0,     0   },
 { DK, "TV 2 News"                     , 0x4509, 0x29, 0x9,  0,    0,    0,     0   },
 { DK, "TV2 DK"                        , 0x450A, 0,    0,    0,    0,    0,     0   },
 { SE, "SVT Test Txmns"                , 0x4600, 0x4E, 0,    0x3E, 0,    0,     0   },
 { SE, "SVT 1"                         , 0x4601, 0x4E, 0x1,  0x3E, 0x1,  0,     0   },
 { SE, "SVT 2"                         , 0x4602, 0x4E, 0x2,  0x3E, 0x2,  0,     0   },
 { SE, "SVT"                           , 0x4603, 0x4E, 0x3,  0x3E, 0x3,  0,     0   },
 { SE, "SVT"                           , 0x4604, 0x4E, 0x4,  0x3E, 0x4,  0,     0   },
 { SE, "SVT"                           , 0x4605, 0x4E, 0x5,  0x3E, 0x5,  0,     0   },
 { SE, "SVT"                           , 0x4606, 0x4E, 0x6,  0x3E, 0x6,  0,     0   },
 { SE, "SVT"                           , 0x4607, 0x4E, 0x7,  0x3E, 0x7,  0,     0   },
 { SE, "SVT"                           , 0x4608, 0x4E, 0x8,  0x3E, 0x8,  0,     0   },
 { SE, "SVT"                           , 0x4609, 0x4E, 0x9,  0x3E, 0x9,  0,     0   },
 { SE, "SVT"                           , 0x460A, 0x4E, 0xA,  0x3E, 0xA,  0,     0   },
 { SE, "SVT"                           , 0x460B, 0x4E, 0xB,  0x3E, 0xB,  0,     0   },
 { SE, "SVT"                           , 0x460C, 0x4E, 0xC,  0x3E, 0xC,  0,     0   },
 { SE, "SVT"                           , 0x460D, 0x4E, 0xD,  0x3E, 0xD,  0,     0   },
 { SE, "SVT"                           , 0x460E, 0x4E, 0xE,  0x3E, 0xE,  0,     0   },
 { SE, "SVT"                           , 0x460F, 0x4E, 0xF,  0x3E, 0xF,  0,     0   },
 { SE, "TV 4"                          , 0x4640, 0x4E, 0x40, 0x3E, 0x40, 0,     0   },
 { SE, "TV 4"                          , 0x4641, 0x4E, 0x41, 0x3E, 0x41, 0,     0   },
 { SE, "TV 4"                          , 0x4642, 0x4E, 0x42, 0x3E, 0x42, 0,     0   },
 { SE, "TV 4"                          , 0x4643, 0x4E, 0x43, 0x3E, 0x43, 0,     0   },
 { SE, "TV 4"                          , 0x4644, 0x4E, 0x44, 0x3E, 0x44, 0,     0   },
 { SE, "TV 4"                          , 0x4645, 0x4E, 0x45, 0x3E, 0x45, 0,     0   },
 { SE, "TV 4"                          , 0x4646, 0x4E, 0x46, 0x3E, 0x46, 0,     0   },
 { SE, "TV 4"                          , 0x4647, 0x4E, 0x47, 0x3E, 0x47, 0,     0   },
 { SE, "TV 4"                          , 0x4648, 0x4E, 0x48, 0x3E, 0x48, 0,     0   },
 { SE, "TV 4"                          , 0x4649, 0x4E, 0x49, 0x3E, 0x49, 0,     0   },
 { SE, "TV 4"                          , 0x464A, 0x4E, 0x4A, 0x3E, 0x4A, 0,     0   },
 { SE, "TV 4"                          , 0x464B, 0x4E, 0x4B, 0x3E, 0x4B, 0,     0   },
 { SE, "TV 4"                          , 0x464C, 0x4E, 0x4C, 0x3E, 0x4C, 0,     0   },
 { SE, "TV 4"                          , 0x464D, 0x4E, 0x4D, 0x3E, 0x4D, 0,     0   },
 { SE, "TV 4"                          , 0x464E, 0x4E, 0x4E, 0x3E, 0x4E, 0,     0   },
 { SE, "TV 4"                          , 0x464F, 0x4E, 0x4F, 0x3E, 0x4F, 0,     0   },
 { NO, "NRK1"                          , 0x4701, 0,    0,    0,    0,    0,     0   },
 { NO, "TV 2"                          , 0x4702, 0,    0,    0,    0,    0,     0   },
 { NO, "NRK2"                          , 0x4703, 0,    0,    0,    0,    0,     0   },
 { NO, "TV Norge"                      , 0x4704, 0,    0,    0,    0,    0,     0   },
 { NO, "Discovery Nordic"              , 0x4720, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP1"                          , 0x4801, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP2"                          , 0x4802, 0,    0,    0,    0,    0,     0   },
 { PL, "TV Polonia"                    , 0x4810, 0,    0,    0,    0,    0,     0   },
 { PL, "TVN"                           , 0x4820, 0,    0,    0,    0,    0,     0   },
 { PL, "TVN Siedem"                    , 0x4821, 0,    0,    0,    0,    0,     0   },
 { PL, "TVN24"                         , 0x4822, 0,    0,    0,    0,    0,     0   },
 { PL, "Discovery Poland"              , 0x4830, 0,    0,    0,    0,    0,     0   },
 { PL, "Animal Planet"                 , 0x4831, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Warszawa"                  , 0x4880, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Bialystok"                 , 0x4881, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Bydgoszcz"                 , 0x4882, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Gdansk"                    , 0x4883, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Katowice"                  , 0x4884, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Krakow"                    , 0x4886, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Lublin"                    , 0x4887, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Lodz"                      , 0x4888, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Rzeszow"                   , 0x4890, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Poznan"                    , 0x4891, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Szczecin"                  , 0x4892, 0,    0,    0,    0,    0,     0   },
 { PL, "TVP Wroclaw"                   , 0x4893, 0,    0,    0,    0,    0,     0   },
 { DE, "Das Erste"                     , 0x4901, 0x1D, 0x01, 0x3D, 0,    0xDC1, 101 }, /* 8/30/2 ID's for GERMANY are all _guessed_ */
 { DE, "ZDF"                           , 0,      0,    0,    0x3D, 0x40, 0,     102 },
 { DE, "ZDF"                           , 0,      0,    0,    0x3D, 0x41, 0,     102 },
 { DE, "ZDF"                           , 0x4902, 0x1D, 0x02, 0x3D, 0x42, 0xDC2, 102 },
 { DE, "ZDFinfokanal"                  , 0x4902, 0x1D, 0x02, 0x3D, 0,    0,     0   },
 { DE, "ZDFtheaterkanal"               , 0x4902, 0x1D, 0x02, 0x3D, 0,    0,     0   },
 { DE, "zdf_neo"                       , 0x4902, 0x1D, 0x02, 0x3D, 0,    0,     0   },
 { DE, "neo/Kika"                      , 0x4902, 0x1D, 0x02, 0x3D, 0,    0,     0   },
 { DE, "OK54 B�rgerrundfunk Trier"     , 0x4904, 0x1D, 0x04, 0x3D, 0,    0xD04, 0   },
 { DE, "Phoenix"                       , 0x4908, 0x1D, 0x08, 0x3D, 0,    0xDC8, 0   },
 { DE, "arte"                          , 0x490A, 0x1D, 0x0A, 0x3D, 0,    0xD85, 205 },
 { DE, "VOX"                           , 0x490C, 0x1D, 0x0C, 0x3D, 0,    0xD8E, 214 },
 { DE, "Phoenix"                       , 0x4918, 0x1D, 0x18, 0x3D, 0,    0xDC8, 108 },
 { DE, "hr-fernsehen"                  , 0x493F, 0x1D, 0x3F, 0x3D, 0,    0xDCF, 115 }, /* hmm.. */
 { DE, "Einsfestival"                  , 0x4941, 0x1D, 0x41, 0x3D, 0,    0xD41, 301 },
 { DE, "EinsPlus"                      , 0x4942, 0x1D, 0x42, 0x3D, 0,    0xD42, 302 },
 { DE, "EinsExtra"                     , 0x4943, 0x1D, 0x43, 0x3D, 0,    0xD43, 303 },
 { DE, "BR-alpha"                      , 0x4944, 0x1D, 0x44, 0x3D, 0x44, 0xD44, 304 },
 { DE, "Das Erste"                     , 0x4981, 0x1D, 0x81, 0x3D, 0,    0,     0   },
 { DE, "rbb Berlin/Brandenburg"        , 0x4982, 0x1D, 0x82, 0x3D, 0,    0xDDC, 128 },
 { DE, "rbb Berlin/Brandenburg"        , 0x4982, 0x1D, 0x82, 0x3D, 0,    0xDDC, 202 },
 { DE, "rbb Berlin/Brandenburg"        , 0x4982, 0x1D, 0x82, 0x3D, 0,    0xD82, 0   },
 { DE, "1-2-3.TV"                      , 0x49BD, 0x1D, 0xBD, 0x3D, 0,    0xD77, 355 },
 { DE, "TELE 5"                        , 0x49BE, 0x1D, 0xBE, 0x3D, 0,    0xD78, 356 },
 { DE, "HSE24"                         , 0x49BF, 0x1D, 0xBF, 0x3D, 0,    0xD7F, 363 },
 { DE, "RBB-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD81, 201 },
 { DE, "Das Erste"                     , 0x49C1, 0x1D, 0xC1, 0x3D, 0,    0,     0   },
 { DE, "ARD/ZDF"                       , 0x49C3, 0x1D, 0xC3, 0x3D, 0,    0xDC3, 0   },
 { DE, "Das Erste"                     , 0x49C4, 0x1D, 0xC4, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49C5, 0x1D, 0xC5, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49C6, 0x1D, 0xC6, 0x3D, 0,    0,     0   },
 { DE, "3sat"                          , 0x49C7, 0x1D, 0xC7, 0x3D, 0x47, 0xDC7, 107 },
 { DE, "KiKa"                          , 0x49C9, 0x1D, 0xC9, 0x3D, 0,    0xDC9, 109 },
 { DE, "Das Erste"                     , 0x49CA, 0x1D, 0xCA, 0x3D, 0,    0,     0   },
 { DE, "BR-1"                          , 0,      0x1D, 0,    0x3D, 0,    0xDCA, 110 },
 { DE, "Bayerisches FS"                , 0x49CB, 0x1D, 0xCB, 0x3D, 0x4B, 0xDCB, 111 },
 { DE, "BR3"                           , 0,      0x1D, 0,    0x3D, 0,    0xDCC, 112 },
 { DE, "BR3"                           , 0,      0x1D, 0,    0x3D, 0,    0xDCD, 113 },
 { DE, "Das Erste"                     , 0x49CC, 0x1D, 0xCC, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49CD, 0x1D, 0xCD, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49CE, 0x1D, 0xCE, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D0, 0x1D, 0xD0, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D1, 0x1D, 0xD1, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D2, 0x1D, 0xD2, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D3, 0x1D, 0xD3, 0x3D, 0,    0,     0   },
 { DE, "NDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDD0, 116 },
 { DE, "NDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDD1, 117 },
 { DE, "NDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDD2, 118 },
 { DE, "NDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDD3, 119 },
 { DE, "NDR FS"                        , 0xC016, 0x1D, 0,    0x3D, 0,    0xDD4, 120 }, /* 0xC016?? wtf..? */
 { DE, "NDR FS"                        , 0x16,   0x1D, 0,    0x3D, 0,    0xDF2, 150 }, /* 0x16.. wtf..? */
 { DE, "NDR FS"                        , 0x49D4, 0x1D, 0,    0x3D, 0,    0xDD5, 121 },
 { DE, "NDR FS"                        , 0,      0x1D, 0,    0x3D, 0,    0xDD6, 122 },
 { DE, "NDR FS"                        , 0,      0x1D, 0,    0x3D, 0,    0xDD7, 123 },
 { DE, "NDR FS"                        , 0,      0x1D, 0,    0x3D, 0,    0xDD8, 124 },
 { DE, "RB-1"                          , 0,      0x1D, 0,    0x3D, 0,    0xDD9, 125 },
 { DE, "RB-3"                          , 0,      0x1D, 0,    0x3D, 0,    0xDDA, 126 },
 { DE, "RBB-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDDB, 127 },
 { DE, "SWR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDDD, 129 },
 { DE, "SWR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDDE, 130 },
 { DE, "SR-1"                          , 0,      0x1D, 0,    0x3D, 0,    0xDDF, 131 },
 { DE, "SWR Fernsehen"                 , 0,      0x1D, 0,    0x3D, 0,    0xDE2, 134 },
 { DE, "SWR Fernsehen"                 , 0,      0x1D, 0,    0x3D, 0,    0xDE4, 136 },
 { DE, "SWR-Regional"                  , 0,      0x1D, 0,    0x3D, 0,    0xDEC, 144 },
 { DE, "SWR Fernsehen"                 , 0,      0x1D, 0,    0x3D, 0,    0xDED, 145 },
 { DE, "SWR Fernsehen"                 , 0,      0x1D, 0,    0x3D, 0,    0xDEE, 146 },
 { DE, "SWR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDEF, 147 },
 { DE, "SWR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDF0, 148 },
 { DE, "SWR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDF1, 149 },
 { DE, "Das Erste"                     , 0x49D5, 0x1D, 0xD5, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D6, 0x1D, 0xD6, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D7, 0x1D, 0xD7, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49D8, 0x1D, 0xD8, 0x3D, 0,    0,     0   },
 { DE, "RB"                            , 0x49D9, 0x1D, 0xD9, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49DA, 0x1D, 0xDA, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49DB, 0x1D, 0xDB, 0x3D, 0,    0,     0   },
 { DE, "SFB"                           , 0x49DC, 0x1D, 0xDC, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49DD, 0x1D, 0xDD, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49DE, 0x1D, 0xDE, 0x3D, 0,    0,     0   },
 { DE, "SR"                            , 0x49DF, 0x1D, 0xDF, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49E0, 0x1D, 0xE0, 0x3D, 0,    0,     0   },
 { DE, "S�dwest BW/RP"                 , 0x49E1, 0x1D, 0xE1, 0x3D, 0,    0xDE1, 133 },
 { DE, "S�dwest BW/RP"                 , 0x49E1, 0x1D, 0xE1, 0x3D, 0,    0xDE3, 135 },
 { DE, "Das Erste"                     , 0x49E2, 0x1D, 0xE2, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49E3, 0x1D, 0xE3, 0x3D, 0,    0,     0   },
 { DE, "SWR Fernsehen RP"              , 0x49E4, 0x1D, 0xE4, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49E5, 0x1D, 0xE5, 0x3D, 0,    0,     0   },
 { DE, "WDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDE5, 137 },
 { DE, "WDR"                           , 0x49E6, 0x1D, 0,    0x3D, 0x66, 0xDE6, 138 },
 { DE, "WDR"                           , 0,      0x1D, 0,    0x3D, 0,    0xDE7, 139 },
 { DE, "WDR"                           , 0,      0x1D, 0,    0x3D, 0,    0xDE8, 140 },
 { DE, "WDR"                           , 0,      0x1D, 0,    0x3D, 0,    0xDE9, 141 },
 { DE, "WDR"                           , 0,      0x1D, 0,    0x3D, 0,    0xDEA, 142 },
 { DE, "WDR"                           , 0,      0x1D, 0,    0x3D, 0,    0xDEB, 143 },
 { DE, "WDR-Dortmund"                  , 0,      0x1D, 0,    0x3D, 0,    0xDF7, 155 },
 { DE, "Das Erste"                     , 0x49E7, 0x1D, 0xE7, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49E8, 0x1D, 0xE8, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49E9, 0x1D, 0xE9, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49EA, 0x1D, 0xEA, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49EB, 0x1D, 0xEB, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49EC, 0x1D, 0xEC, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49ED, 0x1D, 0xED, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49EE, 0x1D, 0xEE, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49EF, 0x1D, 0xEF, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F0, 0x1D, 0xF0, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F1, 0x1D, 0xF1, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F2, 0x1D, 0xF2, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F3, 0x1D, 0xF3, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F4, 0x1D, 0xF4, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F5, 0x1D, 0xF5, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F6, 0x1D, 0xF6, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F7, 0x1D, 0xF7, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F8, 0x1D, 0xF8, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49F9, 0x1D, 0xF9, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49FA, 0x1D, 0xFA, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49FB, 0x1D, 0xFB, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49FC, 0x1D, 0xFC, 0x3D, 0,    0,     0   },
 { DE, "Das Erste"                     , 0x49FD, 0x1D, 0xFD, 0x3D, 0,    0,     0   },
 { DE, "MDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDF3, 151 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0xFE, 0x3D, 0,    0xDF4, 152 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDF5, 153 },
 { DE, "MDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDF6, 154 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDF8, 155 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDF9, 157 },
 { DE, "MDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDFA, 158 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDFB, 159 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDFC, 160 },
 { DE, "MDR-1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDFD, 161 },
 { DE, "MDR"                           , 0x49FE, 0x1D, 0,    0x3D, 0,    0xDFE, 162 },
 { DE, "HR-1"                          , 0,      0x1D, 0,    0x3D, 0,    0xDCE, 114 },
 { DE, "hr-fernsehen"                  , 0x49FF, 0x1D, 0xFF, 0x3D, 0,    0xDCF, 115 },
 { DE, "Parlamentsfernsehen"           , 0,      0x1D, 0,    0x3D, 0,    0xD83, 203 },
 { DE, "1A-Fernsehen"                  , 0,      0x1D, 0,    0x3D, 0,    0xD87, 207 },
 { DE, "VIVA"                          , 0,      0x1D, 0,    0x3D, 0,    0xD88, 208 },
 { DE, "Comedy Central"                , 0,      0x1D, 0,    0x3D, 0,    0xD89, 209 },
 { DE, "n-tv"                          , 0,      0x1D, 0,    0x3D, 0,    0xD8C, 212 },
 { DE, "SPORT1"                        , 0,      0x1D, 0,    0x3D, 0,    0xD8D, 213 },
 { DE, "Eurosport"                     , 0,      0x1D, 0,    0x3D, 0,    0xD91, 217 },
 { DE, "kabel eins"                    , 0,      0x1D, 0,    0x3D, 0,    0xD92, 218 },
 { DE, "ProSieben"                     , 0,      0x1D, 0,    0x3D, 0,    0xD94, 220 },
 { DE, "Premiere"                      , 0,      0x1D, 0,    0x3D, 0,    0xDAC, 244 },
 { DE, "9Live"                         , 0,      0x1D, 0,    0x3D, 0,    0xDBA, 258 },
 { DE, "Deutsche Welle FS Berlin"      , 0,      0x1D, 0,    0x3D, 0,    0xDBB, 259 },
 { DE, "Berlin offener Kanal"          , 0,      0x1D, 0,    0x3D, 0,    0xDBD, 261 },
 { DE, "Berlin-Mix-Channel II"         , 0,      0x1D, 0,    0x3D, 0,    0xDBE, 262 },
 { DE, "Berlin-Mix-Channel I"          , 0,      0x1D, 0,    0x3D, 0,    0xDBF, 263 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD95, 221 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD96, 222 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD97, 223 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD98, 224 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xD99, 225 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDAD, 245 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDAE, 246 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDAF, 247 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB0, 248 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB1, 249 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB2, 250 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB3, 251 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB4, 252 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB5, 253 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB6, 254 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB7, 255 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB8, 256 },
 { DE, "SAT.1"                         , 0,      0x1D, 0,    0x3D, 0,    0xDB9, 257 },
 { DE, "DMAX"                          , 0,      0x1D, 0,    0x3D, 0,    0xD72, 350 },
 { DE, "MTV"                           , 0,      0x1D, 0,    0x3D, 0,    0xD73, 351 },
 { DE, "nickelodeon"                   , 0,      0x1D, 0,    0x3D, 0,    0xD74, 352 },
 { DE, "KDG Info"                      , 0,      0x1D, 0,    0x3D, 0,    0xD75, 353 },
 { DE, "DAS VIERTE"                    , 0,      0x1D, 0,    0x3D, 0,    0xD76, 354 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9A, 226 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9B, 227 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9C, 228 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9D, 229 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9E, 230 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xD9F, 231 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA0, 232 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA1, 233 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA2, 234 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA3, 235 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA4, 236 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA5, 237 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA6, 238 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA7, 239 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA8, 240 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDA9, 241 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDAA, 242 },
 { DE, "RTL Television"                , 0,      0x1D, 0,    0x3D, 0,    0xDAB, 243 }, // see 0x31C0
 { DE, "Channel 21"                    , 0,      0x1D, 0,    0x3D, 0,    0xD79, 357 },
 { DE, "Super RTL"                     , 0,      0x1D, 0,    0x3D, 0,    0xD8A, 210 },
 { DE, "RTL Club"                      , 0,      0x1D, 0,    0x3D, 0,    0xD8B, 211 },
 { DE, "RTL2"                          , 0,      0x1D, 0,    0x3D, 0,    0xD8F, 215 },
 { DE, "RTL2"                          , 0,      0x1D, 0,    0x3D, 0,    0xD90, 216 },
 { DE, "N24"                           , 0,      0x1D, 0,    0x3D, 0,    0xD7A, 358 },
 { DE, "TV.Berlin"                     , 0,      0x1D, 0,    0x3D, 0,    0xD7B, 359 },
 { DE, "ONYX"                          , 0,      0x1D, 0,    0x3D, 0,    0xD7C, 360 },
 { GB, "MTV"                           , 0x4D54, 0x2C, 0x14, 0x3C, 0x14, 0,     0   },
 { GB, "MTV"                           , 0x4D55, 0x2C, 0x33, 0x3C, 0x33, 0,     0   },
 { GB, "MTV"                           , 0x4D56, 0x2C, 0x36, 0x3C, 0x36, 0,     0   },
 { GB, "VH-1"                          , 0x4D57, 0x2C, 0x22, 0x3C, 0x22, 0,     0   },
 { GB, "VH-1"                          , 0x4D58, 0x2C, 0x20, 0x3C, 0x20, 0,     0   },
 { GB, "VH-1"                          , 0x4D59, 0x2C, 0x21, 0x3C, 0x21, 0,     0   },
 { GB, "GRANADA PLUS"                  , 0x4D5A, 0x5B, 0xF4, 0x3B, 0x74, 0,     0   },
 { GB, "GRANADA Timeshare"             , 0x4D5B, 0x5B, 0xF5, 0x3B, 0x75, 0,     0   },
 { GB, "NBC Europe"                    , 0x5343, 0x2C, 0x3,  0x3C, 0x3,  0,     0   },
 { GB, "CENTRAL TV"                    , 0x5699, 0x2C, 0x16, 0x3C, 0x16, 0,     0   },
 { GB, "HTV"                           , 0x5AAF, 0x2C, 0x3F, 0x3C, 0x3F, 0,     0   },
 { GB, "QVC"                           , 0x5C33, 0,    0,    0,    0,    0,     0   },
 { GB, "QVC"                           , 0x5C34, 0,    0,    0,    0,    0,     0   },
 { GB, "QVC"                           , 0x5C39, 0,    0,    0,    0,    0,     0   },
 { GB, "QVC UK"                        , 0x5C44, 0,    0,    0,    0,    0,     0   },
 { DE, "QVC"                           , 0x5C49, 0,    0,    0,    0,    0xD7D, 361 },
 { DK, "DR1"                           , 0x7392, 0x29, 0x1,  0x39, 0x1,  0,     0   },
 { UA, "1+1"                           , 0x7700, 0,    0,    0,    0,    0x7C0, 101 },
 { UA, "1+1"                           , 0x7701, 0,    0,    0,    0,    0x7C0, 101 },
 { UA, "1+1"                           , 0x7702, 0,    0,    0,    0,    0x7C0, 101 },
 { UA, "1+1"                           , 0x7703, 0,    0,    0,    0,    0x7C0, 101 },
 { UA, "M1"                            , 0x7705, 0,    0,    0,    0,    0x7C5, 103 },
 { UA, "ICTV"                          , 0x7707, 0,    0,    0,    0,    0,     0   },
 { UA, "Novy Kanal"                    , 0x7708, 0,    0,    0,    0,    0x7C8, 102 },
 { GB, "CARLTON TV"                    , 0x82DD, 0x2C, 0x1D, 0x3C, 0x1D, 0,     0   },
 { GB, "CARLTON TV"                    , 0x82DE, 0x5B, 0xCF, 0x3B, 0x4F, 0,     0   },
 { GB, "CARLTON TV"                    , 0x82DF, 0x5B, 0xD0, 0x3B, 0x50, 0,     0   },
 { GB, "CARLTON TV"                    , 0x82E0, 0x5B, 0xD1, 0x3B, 0x51, 0,     0   },
 { GB, "CARLTON SELECT"                , 0x82E1, 0x2C, 0x5,  0x3C, 0x5,  0,     0   },
 { GB, "CARLTON SEL."                  , 0x82E2, 0x2C, 0x6,  0x3C, 0x6,  0,     0   },
 { GB, "ULSTER TV"                     , 0x833B, 0x2C, 0x3D, 0x3C, 0x3D, 0,     0   },
 { GB, "LWT"                           , 0x884B, 0x2C, 0xB,  0x3C, 0xB,  0,     0   },
 { GB, "LWT"                           , 0x884C, 0x5B, 0xD9, 0x3B, 0x59, 0,     0   },
 { GB, "LWT"                           , 0x884D, 0x5B, 0xDA, 0x3B, 0x5A, 0,     0   },
 { GB, "LWT"                           , 0x884F, 0x5B, 0xDB, 0x3B, 0x5B, 0,     0   },
 { GB, "LWT"                           , 0x8850, 0x5B, 0xDC, 0x3B, 0x5C, 0,     0   },
 { GB, "LWT"                           , 0x8851, 0x5B, 0xDD, 0x3B, 0x5D, 0,     0   },
 { GB, "LWT"                           , 0x8852, 0x5B, 0xDE, 0x3B, 0x5E, 0,     0   },
 { GB, "LWT"                           , 0x8853, 0x5B, 0xDF, 0x3B, 0x5F, 0,     0   },
 { GB, "LWT"                           , 0x8854, 0x5B, 0xE0, 0x3B, 0x60, 0,     0   },
 { AT, "OKTO"                          , 0x8888, 0,    0,    0,    0,    0,     0   },
 { GB, "NBC Europe"                    , 0x8E71, 0x2C, 0x31, 0x3C, 0x31, 0,     0   },
 { GB, "CNBC Europe"                   , 0x8E72, 0x2C, 0x35, 0x3C, 0x35, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E73, 0x2C, 0x32, 0x3C, 0x32, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E74, 0x2C, 0x2E, 0x3C, 0x2E, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E75, 0x2C, 0x2A, 0x3C, 0x2A, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E76, 0x2C, 0x29, 0x3C, 0x29, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E77, 0x2C, 0x28, 0x3C, 0x28, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E78, 0x2C, 0x26, 0x3C, 0x26, 0,     0   },
 { GB, "NBC Europe"                    , 0x8E79, 0x2C, 0x23, 0x3C, 0x23, 0,     0   },
 { TR, "TRT-1"                         , 0x9001, 0x43, 0x1,  0x33, 0x1,  0,     0   },
 { TR, "TRT-2"                         , 0x9002, 0x43, 0x2,  0x33, 0x2,  0,     0   },
 { TR, "TRT-3"                         , 0x9003, 0x43, 0x3,  0x33, 0x3,  0,     0   },
 { TR, "TRT-4"                         , 0x9004, 0x43, 0x4,  0x33, 0x4,  0,     0   },
 { TR, "TRT-INT"                       , 0x9005, 0x43, 0x5,  0x33, 0x5,  0,     0   },
 { TR, "AVRASYA"                       , 0x9006, 0x43, 0x6,  0x33, 0x6,  0,     0   },
 { TR, "Show TV"                       , 0x9007, 0,    0,    0,    0,    0,     0   },
 { TR, "Cine 5"                        , 0x9008, 0,    0,    0,    0,    0,     0   },
 { TR, "Super Sport"                   , 0x9009, 0,    0,    0,    0,    0,     0   },
 { TR, "ATV"                           , 0x900A, 0,    0,    0,    0,    0,     0   },
 { TR, "KANAL D"                       , 0x900B, 0,    0,    0,    0,    0,     0   },
 { TR, "EURO D"                        , 0x900C, 0,    0,    0,    0,    0,     0   },
 { TR, "EKO TV"                        , 0x900D, 0,    0,    0,    0,    0,     0   },
 { TR, "BRAVO TV"                      , 0x900E, 0,    0,    0,    0,    0,     0   },
 { TR, "GALAKSI TV"                    , 0x900F, 0,    0,    0,    0,    0,     0   },
 { TR, "FUN TV"                        , 0x9010, 0,    0,    0,    0,    0,     0   },
 { TR, "TEMPO TV"                      , 0x9011, 0,    0,    0,    0,    0,     0   },
 { TR, "KANAL D"                       , 0x9012, 0,    0,    0,    0,    0,     0   },
 { TR, "KANAL D"                       , 0x9013, 0,    0,    0,    0,    0,     0   },
 { TR, "TGRT"                          , 0x9014, 0,    0,    0,    0,    0,     0   },
 { TR, "Show TV"                       , 0x9015, 0,    0,    0,    0,    0,     0   },
 { TR, "Show TV"                       , 0x9016, 0,    0,    0,    0,    0,     0   },
 { TR, "Show Euro"                     , 0x9017, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9020, 0,    0,    0,    0,    0,     0   },
 { TR, "STARMAX"                       , 0x9021, 0,    0,    0,    0,    0,     0   },
 { TR, "KANAL 6"                       , 0x9022, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR 4"                        , 0x9023, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR 5"                        , 0x9024, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR 6"                        , 0x9025, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR 7"                        , 0x9026, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR 8"                        , 0x9027, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9028, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9029, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9030, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9031, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9032, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9033, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9034, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9035, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9036, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9037, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9038, 0,    0,    0,    0,    0,     0   },
 { TR, "STAR TV"                       , 0x9039, 0,    0,    0,    0,    0,     0   },
 { GB, "CHANNEL 5 (1)"                 , 0x9602, 0x2C, 0x2,  0x3C, 0x2,  0,     0   },
 { GB, "Nickelodeon UK"                , 0xA460, 0,    0,    0,    0,    0,     0   },
 { GB, "Paramount Comedy Channel UK"   , 0xA465, 0,    0,    0,    0,    0,     0   },
 { GB, "TYNE TEES TV"                  , 0xA82C, 0x2C, 0x2C, 0x3C, 0x2C, 0,     0   },
 { GB, "TYNE TEES TV"                  , 0xA82D, 0x5B, 0xE6, 0x3B, 0x66, 0,     0   },
 { GB, "TYNE TEES TV"                  , 0xA82E, 0x5B, 0xE7, 0x3B, 0x67, 0,     0   },
 { SI, "SLO1"                          , 0xAAE1, 0,    0,    0,    0,    0,     0   },
 { SI, "SLO2"                          , 0xAAE2, 0,    0,    0,    0,    0,     0   },
 { SI, "KC"                            , 0xAAE3, 0,    0,    0,    0,    0,     0   },
 { SI, "TLM"                           , 0xAAE4, 0,    0,    0,    0,    0,     0   },
 { SI, "POP TV"                        , 0xAAE5, 0,    0,    0,    0,    0,     0   },
 { SI, "KANAL A"                       , 0xAAE6, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAE7, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAE8, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAE9, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAEA, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAEB, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAEC, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAED, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAEE, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAEF, 0,    0,    0,    0,    0,     0   },
 { SI, "SLO3"                          , 0xAAF1, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF2, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF3, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF4, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF5, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF6, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF7, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF8, 0,    0,    0,    0,    0,     0   },
 { SI, "future use"                    , 0xAAF9, 0,    0,    0,    0,    0,     0   },
 { GB, "GRANADA TV"                    , 0xADD8, 0x2C, 0x18, 0x3C, 0x18, 0,     0   },
 { GB, "GRANADA TV"                    , 0xADD9, 0x5B, 0xD8, 0x3B, 0x58, 0,     0   },
 { GB, "GMTV"                          , 0xADDC, 0x5B, 0xD2, 0x3B, 0x52, 0,     0   },
 { GB, "GMTV"                          , 0xADDD, 0x5B, 0xD3, 0x3B, 0x53, 0,     0   },
 { GB, "GMTV"                          , 0xADDE, 0x5B, 0xD4, 0x3B, 0x54, 0,     0   },
 { GB, "GMTV"                          , 0xADDF, 0x5B, 0xD5, 0x3B, 0x55, 0,     0   },
 { GB, "GMTV"                          , 0xADE0, 0x5B, 0xD6, 0x3B, 0x56, 0,     0   },
 { GB, "GMTV"                          , 0xADE1, 0x5B, 0xD7, 0x3B, 0x57, 0,     0   },
 { GB, "S4C"                           , 0xB4C7, 0x2C, 0x7,  0x3C, 0x7,  0,     0   },
 { GB, "BORDER TV"                     , 0xB7F7, 0x2C, 0x27, 0x3C, 0x27, 0,     0   },
 { ES, "ETB 1"                         , 0xBA01, 0,    0,    0,    0,    0,     0   },
 { GB, "CHANNEL 5 (4)"                 , 0xC47B, 0x2C, 0x3B, 0x3C, 0x3B, 0,     0   },
 { GB, "FilmFour"                      , 0xC4F4, 0x42, 0xF4, 0x32, 0x74, 0,     0   },
 { GB, "ITV NETWORK"                   , 0xC8DE, 0x2C, 0x1E, 0x3C, 0x1E, 0,     0   },
 { ES, "TV3"                           , 0xCA03, 0,    0,    0,    0,    0,     0   },
 { ES, "C33"                           , 0xCA33, 0,    0,    0,    0,    0,     0   },
 { GB, "MERIDIAN"                      , 0xDD50, 0x2C, 0x10, 0x3C, 0x10, 0,     0   },
 { GB, "MERIDIAN"                      , 0xDD51, 0x5B, 0xE1, 0x3B, 0x61, 0,     0   },
 { GB, "MERIDIAN"                      , 0xDD52, 0x5B, 0xE2, 0x3B, 0x62, 0,     0   },
 { GB, "MERIDIAN"                      , 0xDD53, 0x5B, 0xE3, 0x3B, 0x63, 0,     0   },
 { GB, "MERIDIAN"                      , 0xDD54, 0x5B, 0xE4, 0x3B, 0x64, 0,     0   },
 { GB, "MERIDIAN"                      , 0xDD55, 0x5B, 0xE5, 0x3B, 0x65, 0,     0   },
 { ES, "TVE2"                          , 0xE100, 0,    0,    0,    0,    0,     0   },
 { ES, "TVE Internacional Europa"      , 0xE200, 0,    0,    0,    0,    0,     0   },
 { ES, "Tele5"                         , 0xE500, 0x1F, 0xE5, 0,    0,    0,     0   },
 { ES, "Tele5 Estrellas"               , 0xE501, 0x1F, 0xE6, 0,    0,    0,     0   },
 { ES, "Tele5 Sport"                   , 0xE502, 0x1F, 0xE7, 0,    0,    0,     0   },
 { FR, "Eurosport"                     , 0xF101, 0x2F, 0xE2, 0x3F, 0x62, 0,     0   },
 { FR, "Eurosport2"                    , 0xF102, 0x2F, 0xE3, 0x3F, 0x63, 0,     0   },
 { FR, "Eurosportnews"                 , 0xF103, 0x2F, 0xE4, 0x3F, 0x64, 0,     0   },
 { GB, "HTV"                           , 0xF258, 0x2C, 0x38, 0x3C, 0x38, 0,     0   },
 { GB, "GRAMPIAN TV"                   , 0xF33A, 0x2C, 0x3A, 0x3C, 0x3A, 0,     0   },
 { FR, "TV5"                           , 0xF500, 0x2F, 0xE5, 0x3F, 0x65, 0,     0   },
 { FR, "TV5MONDE"                      , 0xF5C0, 0,    0,    0,    0,    0,     0   },
 { GB, "SCOTTISH TV"                   , 0xF9D2, 0x2C, 0x12, 0x3C, 0x12, 0,     0   },
 { IT, "Rete 4"                        , 0xFA04, 0,    0,    0,    0,    0,     0   },
 { IT, "Canale 5"                      , 0xFA05, 0,    0,    0,    0,    0,     0   },
 { IT, "Italia 1"                      , 0xFA06, 0,    0,    0,    0,    0,     0   },
 { GB, "YORKSHIRE TV"                  , 0xFA2C, 0x2C, 0x2D, 0x3C, 0x2D, 0,     0   },
 { GB, "YORKSHIRE TV"                  , 0xFA2D, 0x5B, 0xEA, 0x3B, 0x6A, 0,     0   },
 { GB, "YORKSHIRE TV"                  , 0xFA2E, 0x5B, 0xEB, 0x3B, 0x6B, 0,     0   },
 { GB, "YORKSHIRE TV"                  , 0xFA2F, 0x5B, 0xEC, 0x3B, 0x6C, 0,     0   },
 { GB, "YORKSHIRE TV"                  , 0xFA30, 0x5B, 0xED, 0x3B, 0x6D, 0,     0   },
 { GB, "ANGLIA TV"                     , 0xFB9C, 0x2C, 0x1C, 0x3C, 0x1C, 0,     0   },
 { GB, "ANGLIA TV"                     , 0xFB9D, 0x5B, 0xCD, 0x3B, 0x4D, 0,     0   },
 { GB, "ANGLIA TV"                     , 0xFB9E, 0x5B, 0xCE, 0x3B, 0x4E, 0,     0   },
 { GB, "ANGLIA TV"                     , 0xFB9F, 0x2C, 0x1F, 0x3C, 0x1F, 0,     0   },
 { GB, "CHANNEL 4"                     , 0xFCD1, 0x2C, 0x11, 0x3C, 0x11, 0,     0   },
 { GB, "CHANNEL TV"                    , 0xFCE4, 0x2C, 0x24, 0x3C, 0x24, 0,     0   },
 { GB, "RACING Ch."                    , 0xFCF3, 0x2C, 0x13, 0x3C, 0x13, 0,     0   },
 { GB, "HISTORY Ch."                   , 0xFCF4, 0x5B, 0xF6, 0x3B, 0x76, 0,     0   },
 { GB, "SCI FI CHANNEL"                , 0xFCF5, 0x2C, 0x15, 0x3C, 0x15, 0,     0   },
 { GB, "SKY TRAVEL"                    , 0xFCF6, 0x5B, 0xF9, 0x3B, 0x79, 0,     0   },
 { GB, "SKY SOAPS"                     , 0xFCF7, 0x2C, 0x17, 0x3C, 0x17, 0,     0   },
 { GB, "SKY SPORTS 2"                  , 0xFCF8, 0x2C, 0x8,  0x3C, 0x8,  0,     0   },
 { GB, "SKY GOLD"                      , 0xFCF9, 0x2C, 0x19, 0x3C, 0x19, 0,     0   },
 { GB, "SKY SPORTS"                    , 0xFCFA, 0x2C, 0x1A, 0x3C, 0x1A, 0,     0   },
 { GB, "MOVIE CHANNEL"                 , 0xFCFB, 0x2C, 0x1B, 0x3C, 0x1B, 0,     0   },
 { GB, "SKY MOVIES PLUS"               , 0xFCFC, 0x2C, 0xC,  0x3C, 0xC,  0,     0   },
 { GB, "SKY NEWS"                      , 0xFCFD, 0x2C, 0xD,  0x3C, 0xD,  0,     0   },
 { GB, "SKY ONE"                       , 0xFCFE, 0x2C, 0xE,  0x3C, 0xE,  0,     0   },
 { GB, "SKY TWO"                       , 0xFCFF, 0x2C, 0xF,  0x3C, 0xF,  0,     0   },
 { FR, "Euronews"                      , 0xFE01, 0x2F, 0xE1, 0x3F, 0x61, 0,     0   },
 /* fuzzy logic extension; only cni_vps here! extend above - not here! */
 { DE, "DMAX.de"                       , 0,      0,    0,    0,    0,    0xD72, 0   },
 { DE, "NICK"                          , 0,      0,    0,    0,    0,    0xD74, 0   },
 { DE, "COMEDY"                        , 0,      0,    0,    0,    0,    0xD89, 0   },
 { DE, "SRTL"                          , 0,      0,    0,    0,    0,    0xD8A, 0   },
 { DE, "RTL II"                        , 0,      0,    0,    0,    0,    0xD8F, 0   },
 { DE, "RTL"                           , 0,      0,    0,    0,    0,    0xDAB, 0   },
 { DE, "ARD"                           , 0,      0,    0,    0,    0,    0xDC1, 0   },
 { DE, "KI.KA"                         , 0,      0,    0,    0,    0,    0xDC9, 0   },
 { DE, "Bayern"                        , 0,      0,    0,    0,    0,    0xDCB, 0   },
 { DE, "hr"                            , 0,      0,    0,    0,    0,    0xDCF, 0   },
 { DE, "NDR"                           , 0,      0,    0,    0,    0,    0xDD4, 0   },
 { DE, "rbb"                           , 0,      0,    0,    0,    0,    0xDDC, 0   },
 };

#define CNI_COUNT (sizeof(cni_codes)/sizeof(cni_code))


const char * cSwReceiver::GetCniNameFormat1() {
  unsigned i;

  if (cni_8_30_1) {
     for (i = 0; i < CNI_COUNT; i++)
         if (cni_codes[i].ni_8_30_1 == cni_8_30_1)
            return cni_codes[i].network;
     if (cni_8_30_2 || cni_X_26 || cni_vps)
        dlog(0, "unknown 8/30/1 cni 0x%.4x (8/30/2 = 0x%.4x; X/26 = 0x%.4x, VPS = 0x%.4x; cr_idx = 0x%.4x) %s",
             cni_8_30_1, cni_8_30_2, cni_X_26, cni_vps, cni_cr_idx, "PLEASE REPORT TO WIRBELSCAN AUTHOR.");
     }
  
  return NULL;
}

const char * cSwReceiver::GetCniNameFormat2() {
  uint8_t  c = cni_8_30_2 >> 8;
  uint8_t  n = cni_8_30_2 & 0xFF;

  if (cni_8_30_2) {
     for (unsigned i = 0; i < CNI_COUNT; i++)
         if ((cni_codes[i].c_8_30_2 == c) && (cni_codes[i].ni_8_30_2 == n))
            return cni_codes[i].network;
     if (cni_8_30_1 || cni_X_26 || cni_vps)
        dlog(0, "unknown 8/30/2 cni 0x%.4x (8/30/1 = 0x%.4x; X/26 = 0x%.4x, VPS = 0x%.4x; cr_idx = 0x%.4x) %s",
             cni_8_30_2, cni_8_30_1, cni_X_26, cni_vps, cni_cr_idx, "PLEASE REPORT TO WIRBELSCAN AUTHOR.");
     }
  
  return NULL;
}

const char * cSwReceiver::GetCniNameVPS() {

  if (cni_vps) {
     for (unsigned i = 0; i < CNI_COUNT; i++)
         if (cni_codes[i].vps__cni == cni_vps)
            return cni_codes[i].network;
     if (cni_8_30_1 || cni_8_30_2 || cni_X_26)
        dlog(0, "unknown VPS cni 0x%.4x (8/30/1 = 0x%.4x; 8/30/2 = 0x%.4x, X/26 = 0x%.4x; cr_idx = 0x%.4x) %s",
             cni_vps, cni_8_30_1, cni_8_30_2, cni_X_26, cni_cr_idx, "PLEASE REPORT TO WIRBELSCAN AUTHOR.");
     }
  
  return NULL;
}

const char * cSwReceiver::GetCniNameX26() {
  uchar a = cni_X_26 >> 8, b = cni_X_26 & 0xff;
 
  if (cni_X_26) {
     for (unsigned i = 0; i < CNI_COUNT; i++)
         if ((cni_codes[i].a_X_26 == a) && (cni_codes[i].b_X_26 == b))
            return cni_codes[i].network;
     if (cni_8_30_1 || cni_8_30_2 || cni_vps)
        dlog(0, "unknown X/26 cni 0x%.4x (8/30/1 = 0x%.4x; 8/30/2 = 0x%.4x, VPS = 0x%.4x; cr_idx = 0x%.4x) %s",
             cni_X_26, cni_8_30_1, cni_8_30_2, cni_vps, cni_cr_idx, "PLEASE REPORT TO WIRBELSCAN AUTHOR.");
     }
  
  return NULL;
}

const char * cSwReceiver::GetCniNameCrIdx() {
  uchar c = cni_cr_idx >> 8, idx = cni_cr_idx & 0xff;
 
  if (cni_cr_idx) {
     for (unsigned i = 0; i < CNI_COUNT; i++)
         if ((cni_codes[i].c_8_30_2 == c) && (cni_codes[i].cr_idx == idx))
            return cni_codes[i].network;
     if (cni_8_30_1 || cni_8_30_2 || cni_vps || cni_X_26)
        if ((cni_cr_idx&255) >= 100) //invalid anyway otherwise.
        dlog(0, "unknown cr_idx %.2X%.3d (8/30/1 = 0x%.4x; 8/30/2 = 0x%.4x, VPS = 0x%.4x; X/26 = 0x%.4x) %s",
             cni_cr_idx>>8, cni_cr_idx&255, cni_8_30_1, cni_8_30_2, cni_vps, cni_X_26, "PLEASE REPORT TO WIRBELSCAN AUTHOR.");
     }
  
  return NULL;
}

void cSwReceiver::UpdatefromName(const char * name) {
  uint8_t len = strlen(name);
  uint16_t cni_vps_id = 0;

  if (! len) return;

  for (unsigned i = 0; i < CNI_COUNT; i++)
      if (strlen(cni_codes[i].network) == len) {
         if (! strcasecmp(cni_codes[i].network, name)) {
            if (cni_codes[i].vps__cni) {
               cni_vps_id = cni_codes[i].vps__cni;
               if (! cni_vps_id)
                  dlog(0, "%s: missing cni_vps for ""%s""", __FUNCTION__, name);
               break;
               }
            }
      }

  if (cni_vps_id) {
     cni_vps = cni_vps_id;
     fuzzy = true;
     len = strlen(GetCniNameVPS());
     strncpy(fuzzy_network, GetCniNameVPS(), len);
     fuzzy_network[len] = 0;
     }
  else {     
     dlog(0, "%s: unknown network name ""%s""", __FUNCTION__, name);
     fuzzy = true;
     strncpy(fuzzy_network, name, len);
     fuzzy_network[len] = 0;
     }
  hits++;
  if (++hits > MAXHITS) stopped = true;
}
