/*
 * pesparse.c : MPEG PES parsing utility
 * Copyright (C) 2011 Edward Hervey <bilboed@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "pesparse.h"

GST_DEBUG_CATEGORY_STATIC (pes_parser_debug);
#define GST_CAT_DEFAULT pes_parser_debug

guint32 contador_tramas;

/**
 * mpegts_parse_pes_header:
 * @data: data to parse (starting from, and including, the sync code)
 * @length: size of @data in bytes
 * @res: PESHeader to fill (only valid with #PES_PARSING_OK.
 *
 * Parses the mpeg-ts PES header located in @data into the @res.
 *
 * Returns: #PES_PARSING_OK if the header was fully parsed and valid,
 * #PES_PARSING_BAD if the header is invalid, or #PES_PARSING_NEED_MORE if more data
 * is needed to properly parse the header.
 */
PESParsingResult
mpegts_parse_pes_header (const guint8 *data, gsize length, PESHeader *res)
{
  PESParsingResult ret = PES_PARSING_NEED_MORE;
  gsize origlength = length;
  const guint8 *origdata = data;
  guint32 val32;
  guint8 val8, flags;

  g_assert (res != NULL);

  /* The smallest valid PES header is 6 bytes (prefix + stream_id + length) */
  if (G_UNLIKELY (length < 6))
    goto need_more_data;

  val32 = GST_READ_UINT32_BE (data);
  data += 4;
  length -= 4;
  if (G_UNLIKELY ((val32 & 0xffffff00) != 0x00000100))
    goto bad_start_code;

  /* Clear the header */
  memset (res, 0, sizeof (PESHeader));
  res->PTS = -1;
  res->DTS = -1;
  res->ESCR = -1;

  res->stream_id = val32 & 0x000000ff;

  res->packet_length = GST_READ_UINT16_BE (data);
  if (res->packet_length)
    res->packet_length += 6;
  data += 2;
  length -= 2;

  GST_LOG ("stream_id : 0x%08x , packet_length : %d", res->stream_id,
      res->packet_length);

  /* Jump if we don't need to parse anything more */
  if (G_UNLIKELY (res->stream_id == 0xbc || res->stream_id == 0xbe
          || res->stream_id == 0xbf || (res->stream_id >= 0xf0
              && res->stream_id <= 0xf2) || res->stream_id == 0xff
          || res->stream_id == 0xf8))
    goto done_parsing;

  if (G_UNLIKELY (length < 3))
    goto need_more_data;

  /* '10'                             2
   * PES_scrambling_control           2
   * PES_priority                     1
   * data_alignment_indicator         1
   * copyright                        1
   * original_or_copy                 1 */
  val8 = *data++;
  if (G_UNLIKELY ((val8 & 0xc0) != 0x80))
    goto bad_marker_1;
  res->scrambling_control = (val8 >> 4) & 0x3;
  res->flags = val8 & 0xf;

  GST_LOG ("scrambling_control 0x%0x", res->scrambling_control);
  GST_LOG ("flags_1: %s%s%s%s%s",
      val8 & 0x08 ? "priority " : "",
      val8 & 0x04 ? "data_alignment " : "",
      val8 & 0x02 ? "copyright " : "",
      val8 & 0x01 ? "original_or_copy " : "", val8 & 0x0f ? "" : "<none>");

  /* PTS_DTS_flags                    2
   * ESCR_flag                        1
   * ES_rate_flag                     1
   * DSM_trick_mode_flag              1
   * additional_copy_info_flag        1
   * PES_CRC_flag                     1
   * PES_extension_flag               1*/
  flags = *data++;
  GST_LOG ("flags_2: %s%s%s%s%s%s%s%s%s",
      flags & 0x80 ? "PTS " : "",
      flags & 0x40 ? "DTS " : "",
      flags & 0x20 ? "ESCR" : "",
      flags & 0x10 ? "ES_rate " : "",
      flags & 0x08 ? "DSM_trick_mode " : "",
      flags & 0x04 ? "additional_copy_info " : "",
      flags & 0x02 ? "CRC " : "",
      flags & 0x01 ? "extension " : "", flags ? "" : "<none>");

  /* PES_header_data_length           8 */
  res->header_size = *data++;
  length -= 3;
  if (G_UNLIKELY (length < res->header_size))
    goto need_more_data;

  res->header_size += 9;        /* We add 9 since that's the offset
                                 * of the field in the header*/
  GST_DEBUG ("header_size : %d", res->header_size);

  /* PTS/DTS */

  /* PTS_DTS_flags == 0x01 is invalid */
  if (G_UNLIKELY ((flags >> 6) == 0x01)) {
    GST_WARNING ("Invalid PTS_DTS_flag (0x01 is forbidden)");
  }

  if ((flags & 0x80) == 0x80) {
    /* PTS */
    if (G_UNLIKELY (length < 5))
      goto need_more_data;

    READ_TS (data, res->PTS, bad_PTS_value);
    length -= 5;
    GST_LOG ("PTS %" G_GUINT64_FORMAT " %" GST_TIME_FORMAT,
        res->PTS, GST_TIME_ARGS (MPEGTIME_TO_GSTTIME (res->PTS)));

  }

  if ((flags & 0x40) == 0x40) {
    /* DTS */
    if (G_UNLIKELY (length < 5))
      goto need_more_data;

    READ_TS (data, res->DTS, bad_DTS_value);
    length -= 5;

    GST_LOG ("DTS %" G_GUINT64_FORMAT " %" GST_TIME_FORMAT,
        res->DTS, GST_TIME_ARGS (MPEGTIME_TO_GSTTIME (res->DTS)));
  }

  if (flags & 0x20) {
    /* ESCR */
    if (G_UNLIKELY (length < 5))
      goto need_more_data;
    READ_TS (data, res->ESCR, bad_ESCR_value);
    length -= 5;

    GST_LOG ("ESCR %" G_GUINT64_FORMAT " %" GST_TIME_FORMAT,
        res->ESCR, GST_TIME_ARGS (PCRTIME_TO_GSTTIME (res->ESCR)));
  }

  if (flags & 0x10) {
    /* ES_rate */
    if (G_UNLIKELY (length < 3))
      goto need_more_data;
    val32 = GST_READ_UINT32_BE (data);
    data += 3;
    length -= 3;
    if (G_UNLIKELY ((val32 & 0x80000100) != 0x80000100))
      goto bad_ES_rate;
    res->ES_rate = ((val32 >> 9) & 0x003fffff) * 50;
    GST_LOG ("ES_rate : %d", res->ES_rate);
  }

  if (flags & 0x08) {
    /* DSM trick mode */
    if (G_UNLIKELY (length < 1))
      goto need_more_data;
    val8 = *data++;
    length -= 1;

    res->trick_mode = val8 >> 5;
    GST_LOG ("trick_mode 0x%x", res->trick_mode);

    switch (res->trick_mode) {
      case PES_TRICK_MODE_FAST_FORWARD:
      case PES_TRICK_MODE_FAST_REVERSE:
        res->intra_slice_refresh = (val8 >> 2) & 0x1;
        res->frequency_truncation = val8 & 0x3;
        /* passthrough */
      case PES_TRICK_MODE_FREEZE_FRAME:
        res->field_id = (val8 >> 3) & 0x3;
        break;
      case PES_TRICK_MODE_SLOW_MOTION:
      case PES_TRICK_MODE_SLOW_REVERSE:
        res->rep_cntrl = val8 & 0x1f;
        break;
      default:
        break;
    }
  }

  if (flags & 0x04) {
    /* additional copy info */
    if (G_UNLIKELY (length < 1))
      goto need_more_data;
    val8 = *data++;
    length -= 1;

    if (G_UNLIKELY (!(val8 & 0x80)))
      goto bad_original_copy_info_marker;
    res->additional_copy_info = val8 & 0x7f;
    GST_LOG ("additional_copy_info : 0x%x", res->additional_copy_info);
  }

  if (flags & 0x02) {
    /* CRC */
    if (G_UNLIKELY (length < 2))
      goto need_more_data;
    res->previous_PES_packet_CRC = GST_READ_UINT16_BE (data);
    GST_LOG ("previous_PES_packet_CRC : 0x%x", res->previous_PES_packet_CRC);
    data += 2;
    length -= 2;
  }


  /* jump if we don't have a PES extension */
  if (!(flags & 0x01))
    goto stuffing_byte;

  if (G_UNLIKELY (length < 1))
    goto need_more_data;

  /* PES extension */
  flags = *data++;
  length -= 1;
  GST_DEBUG ("PES_extension_flag: %s%s%s%s%s%s",
      flags & 0x80 ? "PES_private_data " : "",
      flags & 0x40 ? "pack_header_field " : "",
      flags & 0x20 ? "program_packet_sequence_counter " : "",
      flags & 0x10 ? "P-STD_buffer " : "",
      flags & 0x01 ? "PES_extension_flag_2" : "", flags & 0xf1 ? "" : "<none>");

  if (flags & 0x80) {
    /* PES_private data */
    if (G_UNLIKELY (length < 16))
      goto need_more_data;
    res->private_data = data;
    GST_MEMDUMP ("private_data", data, 16);
    data += 16;
    length -= 16;
  }

  if (flags & 0x40) {
    /* pack_header_field */
    if (G_UNLIKELY (length < 1))
      goto need_more_data;

    val8 = *data++;
    length -= 1;
    if (G_UNLIKELY (length < val8))
      goto need_more_data;
    res->pack_header_size = val8;
    res->pack_header = data;

    GST_MEMDUMP ("Pack header data", res->pack_header, res->pack_header_size);

    data += val8;
    length -= val8;
  }

  if (flags & 0x20) {
    /* sequence counter */
    if (G_UNLIKELY (length < 2))
      goto need_more_data;

    val8 = *data++;
    if (G_UNLIKELY ((val8 & 0x80) != 0x80))
      goto bad_sequence_marker1;
    res->program_packet_sequence_counter = val8 & 0x7f;
    GST_LOG ("program_packet_sequence_counter %d",
        res->program_packet_sequence_counter);

    val8 = *data++;
    if (G_UNLIKELY ((val8 & 0x80) != 0x80))
      goto bad_sequence_marker2;
    res->MPEG1_MPEG2_identifier = (val8 >> 6) & 0x1;
    res->original_stuff_length = val8 & 0x3f;
    GST_LOG ("MPEG1_MPEG2_identifier : %d , original_stuff_length : %d",
        res->MPEG1_MPEG2_identifier, res->original_stuff_length);
    length -= 2;
  }

  if (flags & 0x10) {
    /* P-STD
     * '01'               :  2 bits
     * P-STD_buffer_scale :  1 bit
     * P-STD_buffer_size  : 13 bits
     * */
    if (G_UNLIKELY (length < 2))
      goto need_more_data;
    val8 = *data;
    if (G_UNLIKELY ((val8 & 0xc0) != 0x40))
      goto bad_P_STD_marker;
    /* If P-STD_buffer_scale is 0
     *   multiply by 128 (i.e. << 7),
     * else
     *   multiply by 1024 (i.e. << 10)
     */
    res->P_STD_buffer_size =
        (GST_READ_UINT16_BE (data) & 0x1fff) << ((val8 & 0x20) ? 10 : 7);
    GST_LOG ("P_STD_buffer_size : %d", res->P_STD_buffer_size);
    data += 2;
    length -= 2;
  }

  /* jump if we don't have a PES 2nd extension */
  if (!(flags & 0x01))
    goto stuffing_byte;

  /* Extension flag 2 */
  if (G_UNLIKELY (length < 1))
    goto need_more_data;

  val8 = *data++;
  length -= 1;

  if (!(val8 & 0x80))
    goto bad_extension_marker_2;

  res->extension_field_length = val8 & 0x7f;

  /* Skip empty extensions */
  if (G_UNLIKELY (res->extension_field_length == 0))
    goto stuffing_byte;

  if (G_UNLIKELY (length < res->extension_field_length))
    goto need_more_data;

  flags = *data++;
  res->extension_field_length -= 1;

  if (!(flags & 0x80)) {
    /* Only valid if stream_id_extension_flag == 0x0 */
    res->stream_id_extension = flags;
    GST_LOG ("stream_id_extension : 0x%02x", res->stream_id_extension);
  } else if (!(flags & 0x01)) {
    /* Skip broken streams (that use stream_id_extension with highest bit set
     * for example ...) */
    if (G_UNLIKELY (res->extension_field_length < 5))
      goto stuffing_byte;

    GST_LOG ("TREF field present");
    data += 5;
    res->extension_field_length -= 5;
  }

  /* Extension field data */
  if (res->extension_field_length) {
    res->stream_id_extension_data = data;
    GST_MEMDUMP ("stream_id_extension_data",
        res->stream_id_extension_data, res->extension_field_length);
  }

stuffing_byte:
  /* Go to the expected data start position */
  data = origdata + res->header_size;
  length = origlength - res->header_size;



  /****************************************************/
  /*                     MODIFICACION IIM                                 */
  /****************************************************/

  /*Lectura Tipo de trama segun T-REC-H.264-201304 */
  guint32 leading_zero_8bits;

  if (leading_zero_8bits = *(data + 3) == 000001) {
    ////g_print("leading_zero_8bits: 0x%08x\n",leading_zero_8bits);

    /* 7.3.1 Sintaxis de las unidades NAL
     * 
     * forbidden_zero_bit
     * 
     * nal_ref_idc
     * 
     * nal_unit_type
     */

    guint8 forbidden_zero_bit = *(data + 4);
    ////g_print("forbidden_zero_bit: 0x%02x\n",forbidden_zero_bit );

    //rbsp_byte
    guint8 rbsp_byte = *(data + 5);
    ////g_print("rbsp_byte: 0x%02x\n",rbsp_byte );

    if (leading_zero_8bits = *(data + 9) == 000001) {
      ////g_print("fin tren bits: 0x%08x\n",leading_zero_8bits);
      guint8 slice_header = *(data + 10);

      guint8 first_mb_in_slice = (guint8) ((slice_header & 0xf0));
      ////g_print("first_mb_in_slice: 0x%02x\n",first_mb_in_slice);

      guint8 slice_type = (guint8) (slice_header & 0x0f);
      res->slice_type_nalu = slice_type;

      contador_tramas = contador_tramas + 1;

      if (slice_type == 0x07) {
        //g_print("slice_type: I-slice\n");
        //g_print("Trama nº %" G_GUINT64_FORMAT "\n",contador_tramas);
      }

      /* Cuadro 7-3 – Nombres correspondientes a slice_type
       * 
       *     Type             Description
       * 
       *0     P-slice.        Consists of P-macroblocks (each macro block is predicted using one reference frame) and / or I-macroblocks.
       *1     B-slice.        Consists of B-macroblocks (each macroblock is predicted using one or two reference frames) and / or I-macroblocks.
       *2     I-slice.        Contains only I-macroblocks. Each macroblock is predicted from previously coded blocks of the same slice.
       *3     SP-slice.       Consists of P and / or I-macroblocks and lets you switch between encoded streams.
       *4     SI-slice.       It consists of a special type of SI-macroblocks and lets you switch between encoded streams.
       *5     P-slice.
       *6     B-slice.
       *7     I-slice.
       *8     SP-slice.
       *9     SI-slice.
       */

      /*
         ////g_print("slice_type: 0x%02x\n",slice_type);
         //g_print("- - - - - - - - - -\n");
         if (slice_type == 0x00){//g_print("slice_type: P-slice\n");}
         if (slice_type == 0x01){//g_print("slice_type: B-slice\n");}
         if (slice_type == 0x02){//g_print("slice_type: I-slice\n");}
         if (slice_type == 0x03){//g_print("slice_type: SP-slice\n");}
         if (slice_type == 0x04){//g_print("slice_type: SI-slice\n");}
         if (slice_type == 0x05){//g_print("slice_type: P-slice\n");}
         if (slice_type == 0x06){//g_print("slice_type: B-slice\n");}
         if (slice_type == 0x07){
         //g_print("slice_type: I-slice\n");
         //g_print("PTS %" G_GUINT64_FORMAT " %" GST_TIME_FORMAT"\n",res->PTS, GST_TIME_ARGS (MPEGTIME_TO_GSTTIME (res->PTS)));
         }
         if (slice_type == 0x08){//g_print("slice_type: SP-slice\n");}
         if (slice_type == 0x09){//g_print("slice_type: SI-slice\n");}
         //g_print("- - - - - - - - - -\n");
       */
    }                           // fin tren de bits

  }                             //if 0x000001

  else if (leading_zero_8bits = *(data + 5) == 00000001) {
    //g_print("leading_zero_8bits: 0x%08x\n",leading_zero_8bits );

    guint8 forbidden_zero_bit = *(data + 6);;   // forbidden_zero_bit igual a 0x0
    //g_print("forbidden_zero_bit: 0x%0x\n",forbidden_zero_bit );

    //rbsp_byte
    guint8 rbsp_byte = *(data + 7);
    ////g_print("rbsp_byte: 0x%02x\n",rbsp_byte );

    if (leading_zero_8bits = *(data + 11) == 000001) {
      ////g_print("fin tren bits: 0x%08x\n",leading_zero_8bits);
      guint8 slice_header = *(data + 12);

      guint8 first_mb_in_slice = (guint8) ((slice_header & 0xf0));
      ////g_print("first_mb_in_slice: 0x%02x\n",first_mb_in_slice);

      guint8 slice_type = (guint8) (slice_header & 0x0f);
      /*
       *Type  Description
       *0     P-slice. Consists of P-macroblocks (each macro block is predicted using one reference frame) and / or I-macroblocks.
       *1     B-slice. Consists of B-macroblocks (each macroblock is predicted using one or two reference frames) and / or I-macroblocks.
       *2     I-slice. Contains only I-macroblocks. Each macroblock is predicted from previously coded blocks of the same slice.
       *3     SP-slice. Consists of P and / or I-macroblocks and lets you switch between encoded streams.
       *4     SI-slice. It consists of a special type of SI-macroblocks and lets you switch between encoded streams.
       *5     P-slice.
       *6     B-slice.
       *7     I-slice.
       *8     SP-slice.
       *9     SI-slice.
       */

      /*
         ////g_print("slice_type: 0x%02x\n",slice_type);
         //g_print("- - - - - - - - - -\n");
         if (slice_type == 0x00){//g_print("slice_type: P-slice\n");}
         if (slice_type == 0x01){//g_print("slice_type: B-slice\n");}
         if (slice_type == 0x02){//g_print("slice_type: I-slice\n");}
         if (slice_type == 0x03){//g_print("slice_type: SP-slice\n");}
         if (slice_type == 0x04){//g_print("slice_type: SI-slice\n");}
         if (slice_type == 0x05){//g_print("slice_type: P-slice\n");}
         if (slice_type == 0x06){//g_print("slice_type: B-slice\n");}
         if (slice_type == 0x07){//g_print("slice_type: I-slice\n");}
         if (slice_type == 0x08){//g_print("slice_type: SP-slice\n");}
         if (slice_type == 0x09){//g_print("slice_type: SI-slice\n");}
         //g_print("- - - - - - - - - -\n");
       */
    }                           // fin tren de bits
  }                             //else if 00000001

  /*                      FIN MODIFICACION                        */


done_parsing:
  GST_DEBUG ("origlength:%" G_GSIZE_FORMAT ", length:%" G_GSIZE_FORMAT,
      origlength, length);

  res->header_size = origlength - length;
  ret = PES_PARSING_OK;

  return ret;

  /* Errors */
need_more_data:
  GST_DEBUG ("Not enough data to parse PES header");
  return ret;

bad_start_code:
  GST_WARNING ("Wrong packet start code 0x%x != 0x000001xx", val32);
  return PES_PARSING_BAD;

bad_marker_1:
  GST_WARNING ("Wrong '0x10' marker before PES_scrambling_control (0x%02x)",
      val8);
  return PES_PARSING_BAD;

bad_PTS_value:
  GST_WARNING ("bad PTS value");
  return PES_PARSING_BAD;

bad_DTS_value:
  GST_WARNING ("bad DTS value");
  return PES_PARSING_BAD;

bad_ESCR_value:
  GST_WARNING ("bad ESCR value");
  return PES_PARSING_BAD;

bad_ES_rate:
  GST_WARNING ("Invalid ES_rate markers 0x%0x", val32);
  return PES_PARSING_BAD;

bad_original_copy_info_marker:
  GST_WARNING ("Invalid original_copy_info marker bit: 0x%0x", val8);
  return PES_PARSING_BAD;

bad_sequence_marker1:
  GST_WARNING ("Invalid program_packet_sequence_counter marker 0x%0x", val8);
  return PES_PARSING_BAD;

bad_sequence_marker2:
  GST_WARNING ("Invalid program_packet_sequence_counter marker 0x%0x", val8);
  return PES_PARSING_BAD;

bad_P_STD_marker:
  GST_WARNING ("Invalid P-STD_buffer marker 0x%0x", val8);
  return PES_PARSING_BAD;

bad_extension_marker_2:
  GST_WARNING ("Invalid extension_field_2 marker 0x%0x", val8);
  return PES_PARSING_BAD;
}

void
init_pes_parser (void)
{
  GST_DEBUG_CATEGORY_INIT (pes_parser_debug, "pesparser", 0, "MPEG PES parser");
}
