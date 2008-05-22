/*
 *******************************************************************************
 * Copyright (C) 2008, International Business Machines Corporation and         *
 * others. All Rights Reserved.                                                *
 *******************************************************************************
 */
package com.ibm.icu.charset;

import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.IntBuffer;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.CoderResult;

import com.ibm.icu.text.UTF16;
import com.ibm.icu.text.UnicodeSet;

public class CharsetHZ extends CharsetICU {

    private static final int UCNV_TILDE = 0x7E; /* ~ */
    private static final int UCNV_OPEN_BRACE = 0x7B; /* { */
    private static final int UCNV_CLOSE_BRACE = 0x7D; /* } */
    private static final byte[] SB_ESCAPE = new byte[] { 0x7E, 0x7D };
    private static final byte[] DB_ESCAPE = new byte[] { 0x7E, 0x7B };
    private static final byte[] TILDE_ESCAPE = new byte[] { 0x7E, 0x7E };
    private static final byte[] fromUSubstitution = new byte[] { (byte) 0x1A };

    private CharsetMBCS gbCharset;
    private boolean isEmptySegment;

    public CharsetHZ(String icuCanonicalName, String canonicalName, String[] aliases) {
        super(icuCanonicalName, canonicalName, aliases);
        gbCharset = (CharsetMBCS) new CharsetProviderICU().charsetForName("GBK");

        maxBytesPerChar = 4;
        minBytesPerChar = 1;
        maxCharsPerByte = 1;
        
        isEmptySegment = false;
    }

    class CharsetDecoderHZ extends CharsetDecoderICU {
        CharsetMBCS.CharsetDecoderMBCS gbDecoder;
        boolean isStateDBCS = false;

        public CharsetDecoderHZ(CharsetICU cs) {
            super(cs);
            gbDecoder = (CharsetMBCS.CharsetDecoderMBCS) gbCharset.newDecoder();
        }

        protected void implReset() {
            super.implReset();
            gbDecoder.implReset();

            isStateDBCS = false;
            isEmptySegment = false;
        }

        protected CoderResult decodeLoop(ByteBuffer source, CharBuffer target, IntBuffer offsets, boolean flush) {
            byte[] tempBuf = new byte[2];
            int targetUniChar = 0;
            int mySourceChar = 0;

            if (!source.hasRemaining())
                return CoderResult.UNDERFLOW;
            else if (!target.hasRemaining())
                return CoderResult.OVERFLOW;

            while (source.hasRemaining()) {

                if (target.hasRemaining()) {

                    // get the byte as unsigned
                    mySourceChar = source.get() & 0xff;

                    if (mode == UCNV_TILDE) {
                        /* second byte after ~ */
                        mode = 0;
                        switch (mySourceChar) {
                        case 0x0A:
                            /* no output for ~\n (line-continuation marker) */
                            continue;
                        case UCNV_TILDE:
                            if (offsets != null) {
                                offsets.put(source.position() - 2);
                            }
                            target.put((char) mySourceChar);
                            continue;
                        case UCNV_OPEN_BRACE:
                        case UCNV_CLOSE_BRACE:
                            isStateDBCS = (mySourceChar == UCNV_OPEN_BRACE);
                            if (isEmptySegment) {
                                isEmptySegment = false; /* we are handling it, reset to avoid future spurious errors */
                                this.toUBytesArray[0] = UCNV_TILDE;
                                this.toUBytesArray[1] = (byte)mySourceChar;
                                this.toULength = 2;
                                return CoderResult.malformedForLength(1);
                            }
                            isEmptySegment = true;
                            continue;
                        default:
                            /*
                             * if the first byte is equal to TILDE and the trail byte is not a valid byte then it is an
                             * error condition
                             */
                            mySourceChar |= 0x7e00;
                            targetUniChar = 0xffff;
                            isEmptySegment = false; /* different error here, reset this to avoid spurious future error */ 
                            break;
                        }
                    } else if (isStateDBCS) {
                        if (toUnicodeStatus == 0) {
                            /* lead byte */
                            if (mySourceChar == UCNV_TILDE) {
                                mode = UCNV_TILDE;
                            } else {
                                /*
                                 * add another bit to distinguish a 0 byte from not having seen a lead byte
                                 */
                                toUnicodeStatus = mySourceChar | 0x100;
                                isEmptySegment = false; /* the segment has something, either valid or will produce a different error, so reset this */ 
                            }
                            continue;
                        } else {
                            /* trail byte */
                            int leadByte = toUnicodeStatus & 0xff;
                            if (0x21 <= leadByte && leadByte <= 0x7d && 0x21 <= mySourceChar && mySourceChar <= 0x7e) {
                                tempBuf[0] = (byte) (leadByte + 0x80);
                                tempBuf[1] = (byte) (mySourceChar + 0x80);
                                targetUniChar = gbDecoder.simpleGetNextUChar(ByteBuffer.wrap(tempBuf), super.isFallbackUsed());
                            } else {
                                targetUniChar = 0xffff;
                            }
                            /*
                             * add another bit so that the code below writes 2 bytes in case of error
                             */
                            mySourceChar |= 0x10000 | (leadByte << 8);
                            toUnicodeStatus = 0;
                        }
                    } else {
                        if (mySourceChar == UCNV_TILDE) {
                            mode = UCNV_TILDE;
                            continue;
                        } else if (mySourceChar <= 0x7f) {
                            targetUniChar = mySourceChar; /* ASCII */
                            isEmptySegment = false; /* the segment has something valid */
                        } else {
                            targetUniChar = 0xffff;
                            isEmptySegment = false; /* different error here, reset this to avoid spurious future error */
                        }
                    }

                    if (targetUniChar < 0xfffe) {
                        if (offsets != null) {
                            offsets.put(source.position() - 1 - (isStateDBCS ? 1 : 0));
                        }

                        target.put((char) targetUniChar);
                    } else /* targetUniChar >= 0xfffe */{
                        if (mySourceChar > 0xff) {
                            toUBytesArray[toUBytesBegin + 0] = (byte) (mySourceChar >> 8);
                            toUBytesArray[toUBytesBegin + 1] = (byte) mySourceChar;
                            toULength = 2;
                        } else {
                            toUBytesArray[toUBytesBegin + 0] = (byte) mySourceChar;
                            toULength = 1;
                        }
                        if (targetUniChar == 0xfffe) {
                            return CoderResult.unmappableForLength(toULength);
                        } else {
                            return CoderResult.malformedForLength(toULength);
                        }
                    }
                } else {
                    return CoderResult.OVERFLOW;
                }
            }

            return CoderResult.UNDERFLOW;
        }
    }

    class CharsetEncoderHZ extends CharsetEncoderICU {
        CharsetMBCS.CharsetEncoderMBCS gbEncoder;
        boolean isEscapeAppended = false;
        boolean isTargetUCharDBCS = false;

        public CharsetEncoderHZ(CharsetICU cs) {
            super(cs, fromUSubstitution);
            gbEncoder = (CharsetMBCS.CharsetEncoderMBCS) gbCharset.newEncoder();
        }

        protected void implReset() {
            super.implReset();
            gbEncoder.implReset();

            isEscapeAppended = false;
            isTargetUCharDBCS = false;
        }

        protected CoderResult encodeLoop(CharBuffer source, ByteBuffer target, IntBuffer offsets, boolean flush) {
            int length = 0;
            int[] targetUniChar = new int[] { 0 };
            int mySourceChar = 0;
            boolean oldIsTargetUCharDBCS = isTargetUCharDBCS;

            if (!source.hasRemaining())
                return CoderResult.UNDERFLOW;
            else if (!target.hasRemaining())
                return CoderResult.OVERFLOW;

            if (fromUChar32 != 0 && target.hasRemaining()) {
                CoderResult cr = handleSurrogates(source, (char) fromUChar32);
                return (cr != null) ? cr : CoderResult.unmappableForLength(2);
            }
            /* writing the char to the output stream */
            while (source.hasRemaining()) {
                targetUniChar[0] = MISSING_CHAR_MARKER;
                if (target.hasRemaining()) {

                    mySourceChar = source.get();

                    oldIsTargetUCharDBCS = isTargetUCharDBCS;
                    if (mySourceChar == UCNV_TILDE) {
                        /*
                         * concatEscape(args, &myTargetIndex, &targetLength,"\x7E\x7E",err,2,&mySourceIndex);
                         */
                        concatEscape(source, target, offsets, TILDE_ESCAPE);
                        continue;
                    } else if (mySourceChar <= 0x7f) {
                        length = 1;
                        targetUniChar[0] = mySourceChar;
                    } else {
                        length = gbEncoder.fromUChar32(mySourceChar, targetUniChar, super.isFallbackUsed());

                        /*
                         * we can only use lead bytes 21..7D and trail bytes 21..7E
                         */
                        if (length == 2 && 0xa1a1 <= targetUniChar[0] && targetUniChar[0] <= 0xfdfe
                                && 0xa1 <= (targetUniChar[0] & 0xff) && (targetUniChar[0] & 0xff) <= 0xfe) {
                            targetUniChar[0] -= 0x8080;
                        } else {
                            targetUniChar[0] = MISSING_CHAR_MARKER;
                        }
                    }
                    if (targetUniChar[0] != MISSING_CHAR_MARKER) {
                        isTargetUCharDBCS = (targetUniChar[0] > 0x00FF);
                        if (oldIsTargetUCharDBCS != isTargetUCharDBCS || !isEscapeAppended) {
                            /* Shifting from a double byte to single byte mode */
                            if (!isTargetUCharDBCS) {
                                concatEscape(source, target, offsets, SB_ESCAPE);
                                isEscapeAppended = true;
                            } else { /*
                                         * Shifting from a single byte to double byte mode
                                         */
                                concatEscape(source, target, offsets, DB_ESCAPE);
                                isEscapeAppended = true;

                            }
                        }

                        if (isTargetUCharDBCS) {
                            if (target.hasRemaining()) {
                                target.put((byte) (targetUniChar[0] >> 8));
                                if (offsets != null) {
                                    offsets.put(source.position() - 1);
                                }
                                if (target.hasRemaining()) {
                                    target.put((byte) targetUniChar[0]);
                                    if (offsets != null) {
                                        offsets.put(source.position() - 1);
                                    }
                                } else {
                                    errorBuffer[errorBufferLength++] = (byte) targetUniChar[0];
                                    // *err = U_BUFFER_OVERFLOW_ERROR;
                                }
                            } else {
                                errorBuffer[errorBufferLength++] = (byte) (targetUniChar[0] >> 8);
                                errorBuffer[errorBufferLength++] = (byte) targetUniChar[0];
                                // *err = U_BUFFER_OVERFLOW_ERROR;
                            }

                        } else {
                            if (target.hasRemaining()) {
                                target.put((byte) targetUniChar[0]);
                                if (offsets != null) {
                                    offsets.put(source.position() - 1);
                                }

                            } else {
                                errorBuffer[errorBufferLength++] = (byte) targetUniChar[0];
                                // *err = U_BUFFER_OVERFLOW_ERROR;
                            }
                        }

                    } else {
                        /* oops.. the code point is unassigned */
                        /* Handle surrogates */
                        /* check if the char is a First surrogate */

                        if (UTF16.isSurrogate((char) mySourceChar)) {
                            // use that handy handleSurrogates method everyone's been talking about!
                            CoderResult cr = handleSurrogates(source, (char) mySourceChar);
                            return (cr != null) ? cr : CoderResult.unmappableForLength(2);
                        } else {
                            /* callback(unassigned) for a BMP code point */
                            // *err = U_INVALID_CHAR_FOUND;
                            fromUChar32 = mySourceChar;
                            return CoderResult.unmappableForLength(1);
                        }
                    }
                } else {
                    // *err = U_BUFFER_OVERFLOW_ERROR;
                    return CoderResult.OVERFLOW;
                }
            }

            return CoderResult.UNDERFLOW;
        }

        private CoderResult concatEscape(CharBuffer source, ByteBuffer target, IntBuffer offsets, byte[] strToAppend) {
            CoderResult cr = null;
            for (int i=0; i<strToAppend.length; i++) {
                byte b = strToAppend[i];
                if (target.hasRemaining()) {
                    target.put(b);
                    if (offsets != null)
                        offsets.put(source.position() - 1);
                } else {
                    errorBuffer[errorBufferLength++] = b;
                    cr = CoderResult.OVERFLOW;
                }
            }
            return cr;
        }
    }

    public CharsetDecoder newDecoder() {
        return new CharsetDecoderHZ(this);
    }

    public CharsetEncoder newEncoder() {
        return new CharsetEncoderHZ(this);
    }
    
    void getUnicodeSetImpl( UnicodeSet setFillIn, int which){
        setFillIn.add(0,0x7f);
       // CharsetMBCS mbcshz = (CharsetMBCS)CharsetICU.forNameICU("icu-internal-25546");
        gbCharset.MBCSGetFilteredUnicodeSetForUnicode(gbCharset.sharedData, setFillIn, which, CharsetMBCS.UCNV_SET_FILTER_HZ);
    }
}
