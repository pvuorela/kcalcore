/*  -*- c++ -*-
    kmime_codec_qp.cpp

    KMime, the KDE Internet mail/usenet news message library.
    Copyright (c) 2002 Marc Mutz <mutz@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/
/**
  @file
  This file is part of the API for handling @ref MIME data and
  defines the @ref QuotedPrintable, @ref  RFC2047Q, and
  @ref RFC2231 @ref Codec classes.

  @brief
  Defines the classes QuotedPrintableCodec, Rfc2047QEncodingCodec, and
  Rfc2231EncodingCodec.

  @authors Marc Mutz \<mutz@kde.org\>
*/

#include "kmime_codec_qp.h"
#include "kmime_util.h"

#include <kdebug.h>

#include <cassert>

using namespace KMime;

namespace KMime {

// some helpful functions:

/**
  Converts a 4-bit @p value into its hexadecimal characater representation.
  So input of value [0,15] returns ['0','1',... 'F'].  Input values
  greater than 15 will produce undesired results.
  @param value is an unsigned character containing the 4-bit input value.
*/
static inline char binToHex( uchar value )
{
  if ( value > 9 ) {
    return value + 'A' - 10;
  } else {
    return value + '0';
  }
}

/**
  Returns the high-order 4 bits of an 8-bit value in another 8-bit value.
  @param ch is an unsigned character containing the 8-bit input value.
*/
static inline uchar highNibble( uchar ch )
{
  return ch >> 4;
}

/**
  Returns the low-order 4 bits of an 8-bit value in another 8-bit value.
  @param ch is an unsigned character containing the 8-bit input value.
*/
static inline uchar lowNibble( uchar ch )
{
  return ch & 0xF;
}

/**
  Returns true if the specified value is a not Control character or
  question mark; else true.
  @param ch is an unsigned character containing the 8-bit input value.
*/
static inline bool keep( uchar ch )
{
  // no CTLs, except HT and not '?'
  return !( ( ch < ' ' && ch != '\t' ) || ch == '?' );
}

//
// QuotedPrintableCodec
//

class QuotedPrintableEncoder : public Encoder
{
  char mInputBuffer[16];
  uchar mCurrentLineLength; // 0..76
  uchar mAccu;
  uint mInputBufferReadCursor  : 4; // 0..15
  uint mInputBufferWriteCursor : 4; // 0..15
  enum {
    Never, AtBOL, Definitely
  } mAccuNeedsEncoding    : 2;
  bool mSawLineEnd        : 1;
  bool mSawCR             : 1;
  bool mFinishing         : 1;
  bool mFinished          : 1;
  protected:
  friend class QuotedPrintableCodec;
  QuotedPrintableEncoder( bool withCRLF=false )
    : Encoder( withCRLF ), mCurrentLineLength( 0 ), mAccu( 0 ),
      mInputBufferReadCursor( 0 ), mInputBufferWriteCursor( 0 ),
      mAccuNeedsEncoding( Never ),
      mSawLineEnd( false ), mSawCR( false ), mFinishing( false ),
      mFinished( false ) {}

  bool needsEncoding( uchar ch )
    { return ch > '~' || ( ch < ' ' && ch != '\t' ) || ch == '='; }
  bool needsEncodingAtEOL( uchar ch )
    { return ch == ' ' || ch == '\t'; }
  bool needsEncodingAtBOL( uchar ch )
    { return ch == 'F' || ch == '.' || ch == '-'; }
  bool fillInputBuffer( const char* &scursor, const char * const send );
  bool processNextChar();
  void createOutputBuffer( char* &dcursor, const char * const dend );
  public:
    virtual ~QuotedPrintableEncoder() {}

    bool encode( const char* &scursor, const char * const send,
                 char* &dcursor, const char * const dend );

    bool finish( char* &dcursor, const char * const dend );
};

class QuotedPrintableDecoder : public Decoder
{
  const char mEscapeChar;
  char mBadChar;
  /** @p accu holds the msb nibble of the hexchar or zero. */
  uchar mAccu;
  /** @p insideHexChar is true iff we're inside an hexchar (=XY).
      Together with @ref mAccu, we can build this states:
      @li @p insideHexChar == @p false:
      normal text
      @li @p insideHexChar == @p true, @p mAccu == 0:
      saw the leading '='
      @li @p insideHexChar == @p true, @p mAccu != 0:
      saw the first nibble '=X'
  */
  const bool mQEncoding;
  bool mInsideHexChar;
  bool mFlushing;
  bool mExpectLF;
  bool mHaveAccu;
  /** @p mLastChar holds the first char of an encoded char, so that
      we are able to keep the first char if the second char is invalid. */
  char mLastChar;
  protected:
  friend class QuotedPrintableCodec;
  friend class Rfc2047QEncodingCodec;
  friend class Rfc2231EncodingCodec;
  QuotedPrintableDecoder( bool withCRLF=false,
                          bool aQEncoding=false, char aEscapeChar='=' )
    : Decoder( withCRLF ),
      mEscapeChar( aEscapeChar ),
      mBadChar( 0 ),
      mAccu( 0 ),
      mQEncoding( aQEncoding ),
      mInsideHexChar( false ),
      mFlushing( false ),
      mExpectLF( false ),
      mHaveAccu( false ),
      mLastChar( 0 ) {}
  public:
    virtual ~QuotedPrintableDecoder() {}

    bool decode( const char* &scursor, const char * const send,
                 char* &dcursor, const char * const dend );
    bool finish( char* & dcursor, const char * const dend );
};

class Rfc2047QEncodingEncoder : public Encoder
{
  uchar      mAccu;
  uchar      mStepNo;
  const char mEscapeChar;
  bool       mInsideFinishing : 1;
  protected:
  friend class Rfc2047QEncodingCodec;
  friend class Rfc2231EncodingCodec;
  Rfc2047QEncodingEncoder( bool withCRLF=false, char aEscapeChar='=' )
    : Encoder( withCRLF ),
      mAccu( 0 ), mStepNo( 0 ), mEscapeChar( aEscapeChar ),
      mInsideFinishing( false )
    {
      // else an optimization in ::encode might break.
      assert( aEscapeChar == '=' || aEscapeChar == '%' );
    }

  // this code assumes that isEText( mEscapeChar ) == false!
  bool needsEncoding( uchar ch )
    {
      if ( ch > 'z' ) {
        return true; // {|}~ DEL and 8bit chars need
      }
      if ( !isEText( ch ) ) {
        return true; // all but a-zA-Z0-9!/*+- need, too
      }
      if ( mEscapeChar == '%' && ( ch == '*' || ch == '/' ) ) {
        return true; // not allowed in rfc2231 encoding
      }
      return false;
    }

  public:
    virtual ~Rfc2047QEncodingEncoder() {}

    bool encode( const char* & scursor, const char * const send,
                 char* & dcursor, const char * const dend );
    bool finish( char* & dcursor, const char * const dend );
};

// this doesn't access any member variables, so it can be defined static
// but then we can't call it from virtual functions
static int QuotedPrintableDecoder_maxDecodedSizeFor( int insize, bool withCRLF )
{
  // all chars unencoded:
  int result = insize;
  // but maybe all of them are \n and we need to make them \r\n :-o
  if ( withCRLF )
    result += insize;

  // there might be an accu plus escape
  result += 2;

  return result;
}

Encoder *QuotedPrintableCodec::makeEncoder( bool withCRLF ) const
{
  return new QuotedPrintableEncoder( withCRLF );
}

Decoder *QuotedPrintableCodec::makeDecoder( bool withCRLF ) const
{
  return new QuotedPrintableDecoder( withCRLF );
}

int QuotedPrintableCodec::maxDecodedSizeFor( int insize, bool withCRLF ) const
{
  return QuotedPrintableDecoder_maxDecodedSizeFor(insize, withCRLF);
}

Encoder *Rfc2047QEncodingCodec::makeEncoder( bool withCRLF ) const
{
  return new Rfc2047QEncodingEncoder( withCRLF );
}

Decoder *Rfc2047QEncodingCodec::makeDecoder( bool withCRLF ) const
{
  return new QuotedPrintableDecoder( withCRLF, true );
}

int Rfc2047QEncodingCodec::maxDecodedSizeFor( int insize, bool withCRLF ) const
{
  return QuotedPrintableDecoder_maxDecodedSizeFor(insize, withCRLF);
}

Encoder *Rfc2231EncodingCodec::makeEncoder( bool withCRLF ) const
{
  return new Rfc2047QEncodingEncoder( withCRLF, '%' );
}

Decoder *Rfc2231EncodingCodec::makeDecoder( bool withCRLF ) const
{
  return new QuotedPrintableDecoder( withCRLF, true, '%' );
}

int Rfc2231EncodingCodec::maxDecodedSizeFor( int insize, bool withCRLF ) const
{
  return QuotedPrintableDecoder_maxDecodedSizeFor(insize, withCRLF);
}

/********************************************************/
/********************************************************/
/********************************************************/

bool QuotedPrintableDecoder::decode( const char* &scursor,
                                     const char * const send,
                                     char* &dcursor, const char * const dend )
{
  if ( mWithCRLF ) {
    kWarning() << "CRLF output for decoders isn't yet supported!";
  }

  while ( scursor != send && dcursor != dend ) {
    if ( mFlushing ) {
      // we have to flush chars in the aftermath of an decoding
      // error. The way to request a flush is to
      // - store the offending character in mBadChar and
      // - set mFlushing to true.
      // The supported cases are (H: hexchar, X: bad char):
      // =X, =HX, CR
      // mBadChar is only written out if it is not by itself illegal in
      // quoted-printable (e.g. CTLs, 8Bits).
      // A fast way to suppress mBadChar output is to set it to NUL.
      if ( mInsideHexChar ) {
        // output '='
        *dcursor++ = mEscapeChar;
        mInsideHexChar = false;
      } else if ( mHaveAccu ) {
        // output the high nibble of the accumulator:
        *dcursor++ = mLastChar;
        mHaveAccu = false;
        mAccu = 0;
      } else {
        // output mBadChar
        assert( mAccu == 0 );
        if ( mBadChar ) {
          if ( mBadChar == '=' ) {
            mInsideHexChar = true;
          } else {
            *dcursor++ = mBadChar;
          }
          mBadChar = 0;
        }
        mFlushing = false;
      }
      continue;
    }
    assert( mBadChar == 0 );

    uchar ch = *scursor++;
    uchar value = 255;

    if ( mExpectLF && ch != '\n' ) {
      kWarning() << "QuotedPrintableDecoder:"
        "illegally formed soft linebreak or lonely CR!";
      mInsideHexChar = false;
      mExpectLF = false;
      assert( mAccu == 0 );
    }

    if ( mInsideHexChar ) {
      // next char(s) represent nibble instead of itself:
      if ( ch <= '9' ) {
        if ( ch >= '0' ) {
          value = ch - '0';
        } else {
          switch ( ch ) {
          case '\r':
            mExpectLF = true;
            break;
          case '\n':
            // soft line break, but only if mAccu is NUL.
            if ( !mHaveAccu ) {
              mExpectLF = false;
              mInsideHexChar = false;
              break;
            }
            // else fall through
          default:
            kWarning() << "QuotedPrintableDecoder:"
              "illegally formed hex char! Outputting verbatim.";
            mBadChar = ch;
            mFlushing = true;
          }
          continue;
        }
      } else { // ch > '9'
        if ( ch <= 'F' ) {
          if ( ch >= 'A' ) {
            value = 10 + ch - 'A';
          } else { // [:-@]
            mBadChar = ch;
            mFlushing = true;
            continue;
          }
        } else { // ch > 'F'
          if ( ch <= 'f' && ch >= 'a' ) {
            value = 10 + ch - 'a';
          } else {
            mBadChar = ch;
            mFlushing = true;
            continue;
          }
        }
      }

      assert( value < 16 );
      assert( mBadChar == 0 );
      assert( !mExpectLF );

      if ( mHaveAccu ) {
        *dcursor++ = char( mAccu | value );
        mAccu = 0;
        mHaveAccu = false;
        mInsideHexChar = false;
      } else {
        mHaveAccu = true;
        mAccu = value << 4;
        mLastChar = ch;
      }
    } else { // not mInsideHexChar
      if ( ( ch <= '~' && ch >= ' ' ) || ch == '\t' ) {
        if ( ch == mEscapeChar ) {
          mInsideHexChar = true;
        } else if ( mQEncoding && ch == '_' ) {
          *dcursor++ = char( 0x20 );
        } else {
          *dcursor++ = char( ch );
        }
      } else if ( ch == '\n' ) {
        *dcursor++ = '\n';
        mExpectLF = false;
      } else if ( ch == '\r' ) {
        mExpectLF = true;
      } else {
        //kWarning() << "QuotedPrintableDecoder:" << ch <<
        //  "illegal character in input stream!";
        *dcursor++ = char( ch );
      }
    }
  }

  return scursor == send;
}

bool QuotedPrintableDecoder::finish( char* &dcursor, const char * const dend )
{
  while ( ( mInsideHexChar || mHaveAccu || mFlushing ) && dcursor != dend ) {
    // we have to flush chars
    if ( mInsideHexChar ) {
      // output '='
      *dcursor++ = mEscapeChar;
      mInsideHexChar = false;
    }
    else if ( mHaveAccu ) {
      // output the high nibble of the accumulator:
      *dcursor++ = mLastChar;
      mHaveAccu = false;
      mAccu = 0;
    } else {
      // output mBadChar
      assert( mAccu == 0 );
      if ( mBadChar ) {
        *dcursor++ = mBadChar;
        mBadChar = 0;
      }
      mFlushing = false;
    }
  }

  // return false if we are not finished yet; note that mInsideHexChar is always false
  return !( mHaveAccu || mFlushing );
}

bool QuotedPrintableEncoder::fillInputBuffer( const char* &scursor,
                                              const char * const send ) {
  // Don't read more if there's still a tail of a line in the buffer:
  if ( mSawLineEnd ) {
    return true;
  }

  // Read until the buffer is full or we have found CRLF or LF (which
  // don't end up in the input buffer):
  for ( ; ( mInputBufferWriteCursor + 1 ) % 16 != mInputBufferReadCursor &&
          scursor != send ; mInputBufferWriteCursor++ ) {
    char ch = *scursor++;
    if ( ch == '\r' ) {
      mSawCR = true;
    } else if ( ch == '\n' ) {
      // remove the CR from the input buffer (if any) and return that
      // we found a line ending:
      if ( mSawCR ) {
        mSawCR = false;
        assert( mInputBufferWriteCursor != mInputBufferReadCursor );
        mInputBufferWriteCursor--;
      }
      mSawLineEnd = true;
      return true; // saw CRLF or LF
    } else {
      mSawCR = false;
    }
    mInputBuffer[ mInputBufferWriteCursor ] = ch;
  }
  mSawLineEnd = false;
  return false; // didn't see a line ending...
}

bool QuotedPrintableEncoder::processNextChar()
{

  // If we process a buffer which doesn't end in a line break, we
  // can't process all of it, since the next chars that will be read
  // could be a line break. So we empty the buffer only until a fixed
  // number of chars is left (except when mFinishing, which means that
  // the data doesn't end in newline):
  const int minBufferFillWithoutLineEnd = 4;

  assert( mOutputBufferCursor == 0 );

  int bufferFill =
    int( mInputBufferWriteCursor ) - int( mInputBufferReadCursor ) ;
  if ( bufferFill < 0 ) {
    bufferFill += 16;
  }

  assert( bufferFill >=0 && bufferFill <= 15 );

  if ( !mFinishing && !mSawLineEnd &&
       bufferFill < minBufferFillWithoutLineEnd ) {
    return false;
  }

  // buffer is empty, return false:
  if ( mInputBufferReadCursor == mInputBufferWriteCursor ) {
    return false;
  }

  // Real processing goes here:
  mAccu = mInputBuffer[ mInputBufferReadCursor++ ];
  if ( needsEncoding( mAccu ) ) { // always needs encoding or
    mAccuNeedsEncoding = Definitely;
  } else if ( ( mSawLineEnd || mFinishing ) && // needs encoding at end of line
              bufferFill == 1               && // or end of buffer
              needsEncodingAtEOL( mAccu ) ) {
    mAccuNeedsEncoding = Definitely;
  } else if ( needsEncodingAtBOL( mAccu ) ) {
    mAccuNeedsEncoding = AtBOL;
  } else {
    // never needs encoding
    mAccuNeedsEncoding = Never;
  }

  return true;
}

// Outputs processed (verbatim or hex-encoded) chars and inserts soft
// line breaks as necessary. Depends on processNextChar's directions
// on whether or not to encode the current char, and whether or not
// the current char is the last one in it's input line:
void QuotedPrintableEncoder::createOutputBuffer( char* &dcursor,
                                                 const char * const dend )
{
  const int maxLineLength = 76; // rfc 2045

  assert( mOutputBufferCursor == 0 );

  bool lastOneOnThisLine = mSawLineEnd
                           && mInputBufferReadCursor == mInputBufferWriteCursor;

  int neededSpace = 1;
  if ( mAccuNeedsEncoding == Definitely ) {
    neededSpace = 3;
  }

  // reserve space for the soft hyphen (=)
  if ( !lastOneOnThisLine ) {
    neededSpace++;
  }

  if ( mCurrentLineLength > maxLineLength - neededSpace ) {
    // current line too short, insert soft line break:
    write( '=', dcursor, dend );
    writeCRLF( dcursor, dend );
    mCurrentLineLength = 0;
  }

  if ( Never == mAccuNeedsEncoding ||
       ( AtBOL == mAccuNeedsEncoding && mCurrentLineLength != 0 ) ) {
    write( mAccu, dcursor, dend );
    mCurrentLineLength++;
  } else {
    write( '=', dcursor, dend );
    write( binToHex( highNibble( mAccu ) ), dcursor, dend );
    write( binToHex( lowNibble( mAccu ) ), dcursor, dend );
    mCurrentLineLength += 3;
  }
}

bool QuotedPrintableEncoder::encode( const char* &scursor,
                                     const char * const send,
                                     char* &dcursor, const char * const dend )
{
  // support probing by the caller:
  if ( mFinishing ) {
    return true;
  }

  while ( scursor != send && dcursor != dend ) {
    if ( mOutputBufferCursor && !flushOutputBuffer( dcursor, dend ) ) {
      return scursor == send;
    }

    assert( mOutputBufferCursor == 0 );

    // fill input buffer until eol has been reached or until the
    // buffer is full, whatever comes first:
    fillInputBuffer( scursor, send );

    if ( processNextChar() ) {
      // there was one...
      createOutputBuffer( dcursor, dend );
    } else if ( mSawLineEnd &&
                mInputBufferWriteCursor == mInputBufferReadCursor ) {
      // load a hard line break into output buffer:
      writeCRLF( dcursor, dend );
      // signal fillInputBuffer() we are ready for the next line:
      mSawLineEnd = false;
      mCurrentLineLength = 0;
    } else {
      // we are supposedly finished with this input block:
      break;
    }
  }

  // make sure we write as much as possible and don't stop _writing_
  // just because we have no more _input_:
  if ( mOutputBufferCursor ) {
    flushOutputBuffer( dcursor, dend );
  }

  return scursor == send;

} // encode

bool QuotedPrintableEncoder::finish( char* &dcursor, const char * const dend )
{
  mFinishing = true;

  if ( mFinished ) {
    return flushOutputBuffer( dcursor, dend );
  }

  while ( dcursor != dend ) {
    if ( mOutputBufferCursor && !flushOutputBuffer( dcursor, dend ) ) {
      return false;
    }

    assert( mOutputBufferCursor == 0 );

    if ( processNextChar() ) {
      // there was one...
      createOutputBuffer( dcursor, dend );
    } else if ( mSawLineEnd &&
                mInputBufferWriteCursor == mInputBufferReadCursor ) {
      // load a hard line break into output buffer:
      writeCRLF( dcursor, dend );
      mSawLineEnd = false;
      mCurrentLineLength = 0;
    } else {
      mFinished = true;
      return flushOutputBuffer( dcursor, dend );
    }
  }

  return mFinished && !mOutputBufferCursor;

} // finish

bool Rfc2047QEncodingEncoder::encode( const char* &scursor,
                                      const char * const send,
                                      char* &dcursor, const char * const dend )
{
  if ( mInsideFinishing ) {
    return true;
  }

  while ( scursor != send && dcursor != dend ) {
    uchar value = 0;
    switch ( mStepNo ) {
    case 0:
      // read the next char and decide if and how do encode:
      mAccu = *scursor++;
      if ( !needsEncoding( mAccu ) ) {
        *dcursor++ = char( mAccu );
      } else if ( mEscapeChar == '=' && mAccu == 0x20 ) {
        // shortcut encoding for 0x20 (latin-1/us-ascii SPACE)
        // (not for rfc2231 encoding)
        *dcursor++ = '_';
      } else {
        // needs =XY encoding - write escape char:
        *dcursor++ = mEscapeChar;
        mStepNo = 1;
      }
      continue;
    case 1:
      // extract hi-nibble:
      value = highNibble( mAccu );
      mStepNo = 2;
      break;
    case 2:
      // extract lo-nibble:
      value = lowNibble( mAccu );
      mStepNo = 0;
      break;
    default: assert( 0 );
    }

    // and write:
    *dcursor++ = binToHex( value );
  }

  return scursor == send;
} // encode

#include <QtCore/QString>

bool Rfc2047QEncodingEncoder::finish( char* &dcursor, const char * const dend )
{
  mInsideFinishing = true;

  // write the last bits of mAccu, if any:
  while ( mStepNo != 0 && dcursor != dend ) {
    uchar value = 0;
    switch ( mStepNo ) {
    case 1:
      // extract hi-nibble:
      value = highNibble( mAccu );
      mStepNo = 2;
      break;
    case 2:
      // extract lo-nibble:
      value = lowNibble( mAccu );
      mStepNo = 0;
      break;
    default: assert( 0 );
    }

    // and write:
    *dcursor++ = binToHex( value );
  }

  return mStepNo == 0;
}

} // namespace KMime
