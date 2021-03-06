/**********************************************************************
 *
 *   imap4.cc  - IMAP4rev1 KIOSlave
 *   Copyright (C) 2001-2002  Michael Haeckel <haeckel@kde.org>
 *   Copyright (C) 1999  John Corey <jcorey@fruity.ath.cx>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *   Send comments and bug fixes to jcorey@fruity.ath.cx
 *
 *********************************************************************/

/**
 * @class IMAP4Protocol
 * @note References:
 *   - RFC 2060 - Internet Message Access Protocol - Version 4rev1 - December 1996
 *   - RFC 2192 - IMAP URL Scheme - September 1997
 *   - RFC 1731 - IMAP Authentication Mechanisms - December 1994
 *              (Discusses KERBEROSv4, GSSAPI, and S/Key)
 *   - RFC 2195 - IMAP/POP AUTHorize Extension for Simple Challenge/Response
 *            - September 1997 (CRAM-MD5 authentication method)
 *   - RFC 2104 - HMAC: Keyed-Hashing for Message Authentication - February 1997
 *   - RFC 2086 - IMAP4 ACL extension - January 1997
 *   - http://www.ietf.org/internet-drafts/draft-daboo-imap-annotatemore-05.txt
 *          IMAP ANNOTATEMORE draft - April 2004.
 *
 *
 * Supported URLs:
 * \verbatim
imap://server/
imap://user:pass@server/
imap://user;AUTH=method:pass@server/
imap://server/folder/
 * \endverbatim
 * These URLs cause the following actions (in order):
 *   - Prompt for user/pass, list all folders in home directory
 *   - Uses LOGIN to log in
 *   - Uses AUTHENTICATE to log in
 *   - List messages in folder
 *
 * @note API notes:
 *   Not receiving the required write access for a folder means
 *       ERR_CANNOT_OPEN_FOR_WRITING.
 *  ERR_DOES_NOT_EXIST is reserved for folders.
 */

#include "imap4.h"

#include <QByteArray>
#include <QList>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

extern "C" {
#include <sasl/sasl.h>
}

#include <qbuffer.h>
#include <qdatetime.h>
#include <QRegExp>
#include <kprotocolmanager.h>
#include <kcomponentdata.h>
#include <kmessagebox.h>
#include <kdebug.h>
#include <kio/connection.h>
#include <kio/slaveinterface.h>
#include <klocalizedstring.h>
#include <kmimetype.h>
#include <kcodecs.h>
#include <kde_file.h>

#include "common.h"
#include "kdemacros.h"

#define IMAP_PROTOCOL "imap"
#define IMAP_SSL_PROTOCOL "imaps"
const int ImapPort = 143;
const int ImapsPort = 993;

using namespace KIO;

extern "C"
{
  void sigalrm_handler( int );
  KDE_EXPORT int kdemain( int argc, char **argv );
}

int
kdemain (int argc, char **argv)
{
  kDebug( 7116 ) << "IMAP4::kdemain";

  KComponentData instance( "kio_imap4" );
  if ( argc != 4 ) {
    fprintf( stderr, "Usage: kio_imap4 protocol domain-socket1 domain-socket2\n" );
    ::exit( -1 );
  }

  if ( !initSASL() ) {
    ::exit( -1 );
  }

  //set debug handler

  IMAP4Protocol *slave;
  if ( strcasecmp( argv[1], IMAP_SSL_PROTOCOL ) == 0 ) {
    slave = new IMAP4Protocol( argv[2], argv[3], true );
  } else if ( strcasecmp( argv[1], IMAP_PROTOCOL ) == 0 ) {
    slave = new IMAP4Protocol( argv[2], argv[3], false );
  } else {
    abort();
  }
  slave->dispatchLoop();
  delete slave;

  sasl_done();

  return 0;
}

void
sigchld_handler (int signo)
{
  // A signal handler that calls for example waitpid has to save errno
  // before and restore it afterwards.
  // (cf. https://www.securecoding.cert.org/confluence/display/cplusplus/ERR32-CPP.+Do+not+rely+on+indeterminate+values+of+errno)
  const int save_errno = errno;
  int pid, status;

  while ( signo == SIGCHLD ) {
    pid = waitpid( -1, &status, WNOHANG );
    if ( pid <= 0 ) {
      // Reinstall signal handler, since Linux resets to default after
      // the signal occurred ( BSD handles it different, but it should do
      // no harm ).
      KDE_signal( SIGCHLD, sigchld_handler );
      break;
    }
  }

  errno = save_errno;
}

IMAP4Protocol::IMAP4Protocol (const QByteArray & pool, const QByteArray & app, bool isSSL)
  :TCPSlaveBase ((isSSL ? IMAP_SSL_PROTOCOL : IMAP_PROTOCOL), pool, app, isSSL),
   imapParser (),
   mimeIO (),
   mySSL( isSSL ),
   relayEnabled( false ),
   cacheOutput( false ),
   decodeContent( false ),
   outputBuffer(&outputCache),
   outputBufferIndex(0),
   mProcessedSize( 0 ),
   readBufferLen( 0 ),
   mTimeOfLastNoop( QDateTime() )
{
  readBuffer[0] = 0x00;
}

IMAP4Protocol::~IMAP4Protocol ()
{
  disconnectFromHost();
  kDebug( 7116 ) << "IMAP4: Finishing";
}

void
IMAP4Protocol::get (const KUrl & _url)
{
  if ( !makeLogin() ) {
    return;
  }
  kDebug( 7116 ) << "IMAP4::get -" << _url.prettyUrl();
  QString aBox, aSequence, aType, aSection, aValidity, aDelimiter, aInfo;
  enum IMAP_TYPE aEnum =
    parseURL( _url, aBox, aSection, aType, aSequence, aValidity, aDelimiter, aInfo );
  if ( aEnum != ITYPE_ATTACH ) {
    mimeType( getMimeType( aEnum ) );
  }
  if ( aInfo == "DECODE" ) {
    decodeContent = true;
  }

  if ( aSequence == "0:0" && getState() == ISTATE_SELECT ) {
    CommandPtr cmd = doCommand( imapCommand::clientNoop() );
    completeQueue.removeAll( cmd );
  }

  if ( aSequence.isEmpty() ) {
    aSequence = "1:*";
  }

  mProcessedSize = 0;
  CommandPtr cmd;
  if ( !assureBox( aBox, true ) ) {
    return;
  }

#ifdef USE_VALIDITY
  if ( selectInfo.uidValidityAvailable() && !aValidity.isEmpty() &&
       selectInfo.uidValidity() != aValidity.toULong() ) {
    // this url is stale
    error( ERR_COULD_NOT_READ, _url.prettyUrl() );
    return;
  } else
#endif
  {
    // The "section" specified by the application can be:
    // * empty (which means body, size and flags)
    // * a known keyword, like STRUCTURE, ENVELOPE, HEADER, BODY.PEEK[...]
    //        (in which case the slave has some logic to add the necessary items)
    // * Otherwise, it specifies the exact data items to request. In this case, all
    //        the logic is in the app.

    QString aUpper = aSection.toUpper();
    if ( aUpper.contains( "STRUCTURE" ) ) {
      aSection = "BODYSTRUCTURE";
    } else if ( aUpper.contains( "ENVELOPE" ) ) {
      aSection = "UID RFC822.SIZE FLAGS ENVELOPE";
      if ( hasCapability( "IMAP4rev1" ) ) {
        aSection += " BODY.PEEK[HEADER.FIELDS (REFERENCES)]";
      } else {
        // imap4 does not know HEADER.FIELDS
        aSection += " RFC822.HEADER.LINES (REFERENCES)";
      }
    } else if ( aUpper == "HEADER" ) {
      aSection = "UID RFC822.HEADER RFC822.SIZE FLAGS";
    } else if ( aUpper.contains( "BODY.PEEK[" ) ) {
      if ( aUpper.contains( "BODY.PEEK[]" ) ) {
        if ( !hasCapability( "IMAP4rev1" ) ) { // imap4 does not know BODY.PEEK[]
          aSection.replace( "BODY.PEEK[]", "RFC822.PEEK" );
        }
      }
      aSection.prepend( "UID RFC822.SIZE FLAGS " );
    } else if ( aSection.isEmpty() ) {
      aSection = "UID BODY[] RFC822.SIZE FLAGS";
    }
    if ( aEnum == ITYPE_BOX || aEnum == ITYPE_DIR_AND_BOX ) {
      // write the digest header
      cacheOutput = true;
      outputLine( "Content-Type: multipart/digest; boundary=\"IMAPDIGEST\"\r\n", 55 );
      if ( selectInfo.recentAvailable() ) {
        outputLineStr( "X-Recent: " +
                       QString::number( selectInfo.recent() ) + "\r\n" );
      }
      if ( selectInfo.countAvailable() ) {
        outputLineStr( "X-Count: " +
                       QString::number( selectInfo.count() ) + "\r\n" );
      }
      if ( selectInfo.unseenAvailable() ) {
        outputLineStr( "X-Unseen: " +
                       QString::number( selectInfo.unseen() ) + "\r\n" );
      }
      if ( selectInfo.uidValidityAvailable() ) {
        outputLineStr( "X-uidValidity: " +
                       QString::number( selectInfo.uidValidity() ) + "\r\n" );
      }
      if ( selectInfo.uidNextAvailable() ) {
        outputLineStr( "X-UidNext: " +
                       QString::number( selectInfo.uidNext() ) + "\r\n" );
      }
      if ( selectInfo.flagsAvailable() ) {
        outputLineStr( "X-Flags: " +
                       QString::number( selectInfo.flags() ) + "\r\n" );
      }
      if ( selectInfo.permanentFlagsAvailable() ) {
        outputLineStr( "X-PermanentFlags: " +
                       QString::number( selectInfo.permanentFlags() ) + "\r\n" );
      }
      if ( selectInfo.readWriteAvailable() ) {
        if ( selectInfo.readWrite() ) {
          outputLine( "X-Access: Read/Write\r\n", 22 );
        } else {
          outputLine( "X-Access: Read only\r\n", 21 );
        }
      }
      outputLine( "\r\n", 2 );
      flushOutput( QString() );
      cacheOutput = false;
    }

    if ( aEnum == ITYPE_MSG || ( aEnum == ITYPE_ATTACH && !decodeContent ) ) {
      relayEnabled = true; // normal mode, relay data
    }

    if ( aSequence != "0:0" ) {
      QString contentEncoding;
      if ( aEnum == ITYPE_ATTACH && decodeContent ) {
        // get the MIME header and fill getLastHandled()
        QString mySection = aSection;
        mySection.replace( ']', ".MIME]" );
        cmd = sendCommand( imapCommand::clientFetch( aSequence, mySection ) );
        do {
          while ( !parseLoop() ) {
          }
        } while ( !cmd->isComplete() );
        completeQueue.removeAll( cmd );
        // get the content encoding now because getLastHandled will be cleared
        if ( getLastHandled() && getLastHandled()->getHeader() ) {
          contentEncoding = getLastHandled()->getHeader()->getEncoding();
        }

        // from here on collect the data
        // it is send to the client in flushOutput in one go
        // needed to decode the content
        cacheOutput = true;
      }

      cmd = sendCommand( imapCommand::clientFetch( aSequence, aSection ) );
      int res;
      aUpper = aSection.toUpper();
      do {
        while ( !( res = parseLoop() ) ) {
        }
        if ( res == -1 ) {
          break;
        }

        mailHeader *lastone = 0;
        imapCache *cache = getLastHandled();
        if ( cache ) {
          lastone = cache->getHeader();
        }

        if ( cmd && !cmd->isComplete() ) {
          if ( aUpper.contains( "BODYSTRUCTURE" ) ||
               aUpper.contains( "FLAGS" ) ||
               aUpper.contains( "UID" ) ||
               aUpper.contains( "ENVELOPE" ) ||
               ( aUpper.contains( "BODY.PEEK[0]" ) &&
                ( aEnum == ITYPE_BOX || aEnum == ITYPE_DIR_AND_BOX ) ) ) {
            if ( aEnum == ITYPE_BOX || aEnum == ITYPE_DIR_AND_BOX ) {
              // write the mime header (default is here message/rfc822)
              outputLine( "--IMAPDIGEST\r\n", 14 );
              cacheOutput = true;
              if ( cache->getUid() != 0 ) {
                outputLineStr( "X-UID: " +
                               QString::number( cache->getUid() ) + "\r\n" );
              }
              if ( cache->getSize() != 0 ) {
                outputLineStr( "X-Length: " +
                               QString::number( cache->getSize() ) + "\r\n" );
              }
              if ( !cache->getDate().isEmpty() ) {
                outputLineStr( "X-Date: " +
                               cache->getDate() + "\r\n" );
              }
              if ( cache->getFlags() != 0 ) {
                outputLineStr( "X-Flags: " +
                               QString::number( cache->getFlags() ) + "\r\n" );
              }
            } else {
              cacheOutput = true;
            }
            if ( lastone && !decodeContent ) {
              lastone->outputPart( *this );
            }
            cacheOutput = false;
            flushOutput( contentEncoding );
          }
        } // if not complete
      } while ( cmd && !cmd->isComplete() );
      if ( aEnum == ITYPE_BOX || aEnum == ITYPE_DIR_AND_BOX ) {
        // write the end boundary
        outputLine( "--IMAPDIGEST--\r\n", 16 );
      }

      completeQueue.removeAll( cmd );
    }
  }

  // just to keep everybody happy when no data arrived
  data( QByteArray() );

  finished();
  relayEnabled = false;
  cacheOutput = false;
  kDebug( 7116 ) << "IMAP4::get -  finished";
}

void
IMAP4Protocol::listDir (const KUrl & _url)
{
  kDebug( 7116 ) << "IMAP4::listDir -" << _url.prettyUrl();

  if ( _url.path().isEmpty() ) {
    KUrl url = _url;
    url.setPath( "/" );
    redirection( url );
    finished();
    return;
  }

  QString myBox, mySequence, myLType, mySection, myValidity, myDelimiter, myInfo;
  // parseURL with caching
  enum IMAP_TYPE myType =
    parseURL( _url, myBox, mySection, myLType, mySequence, myValidity, myDelimiter, myInfo, true );

  if ( !makeLogin() ) {
    return;
  }

  if ( myType == ITYPE_DIR || myType == ITYPE_DIR_AND_BOX ) {
    QString listStr = myBox;
    CommandPtr cmd;

    if ( !listStr.isEmpty() && !listStr.endsWith( myDelimiter ) &&
         mySection != "FOLDERONLY" ) {
      listStr += myDelimiter;
    }

    if ( mySection.isEmpty() ) {
      listStr += '%';
    } else if ( mySection == "COMPLETE" ) {
      listStr += '*';
    }
    kDebug( 7116 ) << "IMAP4Protocol::listDir - listStr=" << listStr;
    cmd =
      doCommand( imapCommand::clientList( "", listStr,
                                          ( myLType == "LSUB" || myLType == "LSUBNOCHECK" ) ) );
    if ( cmd->result() == "OK" ) {
      QString mailboxName;
      UDSEntry entry;
      KUrl aURL = _url;
      if ( aURL.path().contains( ';' ) ) {
        aURL.setPath( aURL.path().left( aURL.path().indexOf( ';' ) ) );
      }

      kDebug( 7116 ) << "IMAP4Protocol::listDir - got" << listResponses.count();

      if ( myLType == "LSUB" ) {
        // fire the same command as LIST to check if the box really exists
        QList<imapList> listResponsesSave = listResponses;
        doCommand( imapCommand::clientList( "", listStr, false ) );
        for ( QList< imapList >::Iterator it = listResponsesSave.begin();
            it != listResponsesSave.end(); ++it ) {
          bool boxOk = false;
          for ( QList< imapList >::Iterator it2 = listResponses.begin();
              it2 != listResponses.end(); ++it2 ) {
            if ( ( *it2 ).name() == ( *it ).name() ) {
              boxOk = true;
              // copy the flags from the LIST-command
              ( *it ) = ( *it2 );
              break;
            }
          }
          if ( boxOk ) {
            doListEntry( aURL, myBox, ( *it ), ( mySection != "FOLDERONLY" ) );
          } else { // this folder is dead
            kDebug( 7116 ) << "IMAP4Protocol::listDir - suppress" << ( *it ).name();
          }
        }
        listResponses = listResponsesSave;
      } else { // LIST or LSUBNOCHECK
        for ( QList< imapList >::Iterator it = listResponses.begin();
            it != listResponses.end(); ++it ) {
          doListEntry( aURL, myBox, ( *it ), ( mySection != "FOLDERONLY" ) );
        }
      }
      entry.clear();
      listEntry( entry, true );
    } else {
      error( ERR_CANNOT_ENTER_DIRECTORY, _url.prettyUrl() );
      completeQueue.removeAll( cmd );
      return;
    }
    completeQueue.removeAll( cmd );
  }
  if ( ( myType == ITYPE_BOX || myType == ITYPE_DIR_AND_BOX ) &&
        myLType != "LIST" && myLType != "LSUB" && myLType != "LSUBNOCHECK" ) {
    KUrl aURL = _url;
    aURL.setQuery( QString() );
    const QString encodedUrl = aURL.url( KUrl::LeaveTrailingSlash ); // utf-8

    if ( !_url.query().isEmpty() ) {
      QString query = KUrl::fromPercentEncoding( _url.query().toLatin1() );
      query = query.right( query.length() - 1 );
      if ( !query.isEmpty() ) {
        CommandPtr cmd;

        if ( !assureBox( myBox, true ) ) {
          return;
        }

        if ( !selectInfo.countAvailable() || selectInfo.count() ) {
          cmd = doCommand( imapCommand::clientSearch( query ) );
          if ( cmd->result() != "OK" ) {
            error( ERR_UNSUPPORTED_ACTION, _url.prettyUrl() );
            completeQueue.removeAll( cmd );
            return;
          }
          completeQueue.removeAll( cmd );

          QStringList list = getResults();
          int stretch = 0;

          if ( selectInfo.uidNextAvailable() ) {
            stretch = QString::number( selectInfo.uidNext() ).length();
          }
          UDSEntry entry;
          imapCache fake;

          for ( QStringList::ConstIterator it = list.constBegin(); it != list.constEnd();
               ++it ) {
            fake.setUid( ( *it ).toULong() );
            doListEntry( encodedUrl, stretch, &fake );
          }
          entry.clear();
          listEntry( entry, true );
        }
      }
    } else {
      if ( !assureBox( myBox, true ) ) {
        return;
      }

      kDebug( 7116 ) << "IMAP4: select returned:";
      if ( selectInfo.recentAvailable() ) {
        kDebug( 7116 ) << "Recent:" << selectInfo.recent() << "d";
      }
      if ( selectInfo.countAvailable() ) {
        kDebug( 7116 ) << "Count:" << selectInfo.count() << "d";
      }
      if ( selectInfo.unseenAvailable() ) {
        kDebug( 7116 ) << "Unseen:" << selectInfo.unseen() << "d";
      }
      if ( selectInfo.uidValidityAvailable() ) {
        kDebug( 7116 ) << "uidValidity:" << selectInfo.uidValidity() << "d";
      }
      if ( selectInfo.flagsAvailable() ) {
        kDebug( 7116 ) << "Flags:" << selectInfo.flags() << "d";
      }
      if ( selectInfo.permanentFlagsAvailable() ) {
        kDebug( 7116 ) << "PermanentFlags:" << selectInfo.permanentFlags() << "d";
      }
      if ( selectInfo.readWriteAvailable() ) {
        kDebug( 7116 ) << "Access:" << ( selectInfo.readWrite() ? "Read/Write" : "Read only" );
      }

#ifdef USE_VALIDITY
      if ( selectInfo.uidValidityAvailable() &&
           selectInfo.uidValidity() != myValidity.toULong() ) {
        //redirect
        KUrl newUrl = _url;

        newUrl.setPath( '/' + myBox + ";UIDVALIDITY=" +
                        QString::number( selectInfo.uidValidity() ) );
        kDebug( 7116 ) << "IMAP4::listDir - redirecting to" << newUrl.prettyUrl();
        redirection( newUrl );

      } else
#endif
      if ( selectInfo.count() > 0 ) {
        int stretch = 0;

        if ( selectInfo.uidNextAvailable() ) {
          stretch = QString::number( selectInfo.uidNext() ).length();
        }
        //        kDebug( 7116 ) << selectInfo.uidNext() << "d used to stretch" << stretch;
        UDSEntry entry;

        if ( mySequence.isEmpty() ) {
          mySequence = "1:*";
        }

        bool withSubject = mySection.isEmpty();
        if ( mySection.isEmpty() ) {
          mySection = "UID RFC822.SIZE ENVELOPE";
        }

        bool withFlags = mySection.toUpper().contains("FLAGS") ;
        CommandPtr fetch =
          sendCommand( imapCommand::clientFetch( mySequence, mySection ) );
        imapCache *cache;
        do {
          while ( !parseLoop() ) {
          }

          cache = getLastHandled();

          if ( cache && !fetch->isComplete() ) {
            doListEntry( encodedUrl, stretch, cache, withFlags, withSubject );
          }
        } while ( !fetch->isComplete() );
        entry.clear();
        listEntry( entry, true );
      }
    }
  }
  if ( !selectInfo.alert().isNull() ) {
    if ( !myBox.isEmpty() ) {
      warning( i18n( "Message from %1 while processing '%2': %3", myHost, myBox, selectInfo.alert() ) );
    } else {
      warning( i18n( "Message from %1: %2", myHost, selectInfo.alert() ) );
    }
    selectInfo.setAlert( 0 );
  }

  kDebug( 7116 ) << "IMAP4Protocol::listDir - Finishing listDir";
  finished();
}

void
IMAP4Protocol::setHost (const QString & _host, quint16 _port,
                        const QString & _user, const QString & _pass)
{
  if ( myHost != _host || myPort != _port || myUser != _user || myPass != _pass ) {
    // what's the point of doing 4 string compares to avoid 4 string copies?
    // DF: I guess to avoid calling closeConnection() unnecessarily.
    if ( !myHost.isEmpty() ) {
      closeConnection();
    }
    myHost = _host;
    if ( _port == 0 ) {
        myPort = ( mySSL ) ? ImapsPort : ImapPort;
    } else {
        myPort = _port;
    }
    myUser = _user;
    myPass = _pass;
  }
}

void
IMAP4Protocol::parseRelay (const QByteArray & buffer)
{
  if ( relayEnabled ) {
    // relay data immediately
    data( buffer );
    mProcessedSize += buffer.size();
    processedSize( mProcessedSize );
  } else if ( cacheOutput ) {
    // collect data
    if ( !outputBuffer.isOpen() ) {
      outputBuffer.open( QIODevice::WriteOnly );
    }
    outputBuffer.seek( outputBufferIndex );
    outputBuffer.write( buffer, buffer.size() );
    outputBufferIndex += buffer.size();
  }
}

void
IMAP4Protocol::parseRelay (ulong len)
{
  if ( relayEnabled ) {
    totalSize( len );
  }
}

bool IMAP4Protocol::parseRead(QByteArray & buffer, long len, long relay)
{
  const long int bufLen = 8192;
  char buf[bufLen];
  // FIXME
  while ( buffer.size() < len ) {
    ssize_t readLen = myRead( buf, qMin( len - buffer.size(), bufLen - 1 ) );
    if ( readLen == 0 ) {
      kDebug( 7116 ) << "parseRead: readLen == 0 - connection broken";
      error( ERR_CONNECTION_BROKEN, myHost );
      setState( ISTATE_CONNECT );
      closeConnection();
      return false;
    }
    if ( relay > buffer.size() ) {
      QByteArray relayData;
      ssize_t relbuf = relay - buffer.size();
      int currentRelay = qMin( relbuf, readLen );
      relayData = QByteArray::fromRawData( buf, currentRelay );
      parseRelay( relayData );
      relayData.clear();
    }
    {
      QBuffer stream( &buffer );
      stream.open( QIODevice::WriteOnly );
      stream.seek( buffer.size() );
      stream.write( buf, readLen );
      stream.close();
    }
  }
  return ( buffer.size() == len );
}

bool IMAP4Protocol::parseReadLine (QByteArray & buffer, long relay)
{
  if ( myHost.isEmpty() ) {
    return false;
  }

  while ( true ) {
    ssize_t copyLen = 0;
    if ( readBufferLen > 0 ) {
      while ( copyLen < readBufferLen && readBuffer[copyLen] != '\n' ) {
        copyLen++;
      }
      if ( copyLen < readBufferLen ) {
        copyLen++;
      }
      if ( relay > 0 ) {
        QByteArray relayData;

        if ( copyLen < (ssize_t) relay ) {
          relay = copyLen;
        }
        relayData = QByteArray::fromRawData( readBuffer, relay );
        parseRelay( relayData );
        relayData.clear();
//        kDebug( 7116 ) << "relayed :" << relay << "d";
      }
      // append to buffer
      {
        int oldsize = buffer.size();
        buffer.resize( oldsize + copyLen );
        memcpy( buffer.data() + oldsize, readBuffer, copyLen );
//        kDebug( 7116 ) << "appended" << copyLen << "d got now" << buffer.size();
      }

      readBufferLen -= copyLen;
      if ( readBufferLen ) {
        memmove( readBuffer, &readBuffer[copyLen], readBufferLen );
      }
      if ( buffer[buffer.size() - 1] == '\n' ) {
        return true;
      }
    }
    if ( !isConnected() ) {
      kDebug( 7116 ) << "parseReadLine - connection broken";
      error( ERR_CONNECTION_BROKEN, myHost );
      setState( ISTATE_CONNECT );
      closeConnection();
      return false;
    }
    if ( !waitForResponse( responseTimeout() ) ) {
      error( ERR_SERVER_TIMEOUT, myHost );
      setState( ISTATE_CONNECT );
      closeConnection();
      return false;
    }
    readBufferLen = read( readBuffer, IMAP_BUFFER - 1 );
    if ( readBufferLen == 0 ) {
      kDebug( 7116 ) << "parseReadLine: readBufferLen == 0 - connection broken";
      error( ERR_CONNECTION_BROKEN, myHost );
      setState( ISTATE_CONNECT );
      closeConnection();
      return false;
    }
  }
}

void
IMAP4Protocol::setSubURL (const KUrl & _url)
{
  kDebug( 7116 ) << "IMAP4::setSubURL -" << _url.prettyUrl();
  KIO::TCPSlaveBase::setSubUrl( _url );
}

void
IMAP4Protocol::put (const KUrl & _url, int, KIO::JobFlags)
{
  kDebug( 7116 ) << "IMAP4::put -" << _url.prettyUrl();
//  KIO::TCPSlaveBase::put(_url,permissions,flags)
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  enum IMAP_TYPE aType =
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );

  // see if it is a box
  if ( aType != ITYPE_BOX && aType != ITYPE_DIR_AND_BOX ) {
    if ( aBox[aBox.length() - 1] == '/' ) {
      aBox = aBox.right( aBox.length() - 1 );
    }
    CommandPtr cmd = doCommand( imapCommand::clientCreate( aBox ) );

    if ( cmd->result() != "OK" ) {
      error( ERR_COULD_NOT_WRITE, _url.prettyUrl() );
      completeQueue.removeAll( cmd );
      return;
    }
    completeQueue.removeAll( cmd );
  } else {
    QList < QByteArray* > bufferList;
    int length = 0;

    int result;
    // Loop until we got 'dataEnd'
    do {
      QByteArray *buffer = new QByteArray();
      dataReq();               // Request for data
      result = readData( *buffer );
      if ( result > 0 ) {
        bufferList.append( buffer );
        length += result;
      } else {
        delete buffer;
      }
    } while ( result > 0 );

    if ( result != 0 ) {
      error( ERR_ABORTED, _url.prettyUrl() );
      return;
    }

    CommandPtr cmd = sendCommand( imapCommand::clientAppend( aBox, aSection, length ) );
    while ( !parseLoop() ) {
    }

    // see if server is waiting
    if ( !cmd->isComplete() && !getContinuation().isEmpty() ) {
      bool sendOk = true;
      ulong wrote = 0;

      QByteArray *buffer;
      QListIterator<QByteArray *> it( bufferList );
      // send data to server
      while ( it.hasNext() && sendOk ) {
        buffer = it.next();

        sendOk =
          ( write( buffer->data(), buffer->size() ) ==
           (ssize_t) buffer->size() );
        wrote += buffer->size();
        processedSize( wrote );
        delete buffer;
        if ( !sendOk ) {
          error( ERR_CONNECTION_BROKEN, myHost );
          completeQueue.removeAll( cmd );
          setState( ISTATE_CONNECT );
          closeConnection();
          return;
        }
      }
      parseWriteLine( "" );
      // Wait until cmd is complete, or connection breaks.
      while ( !cmd->isComplete() && getState() != ISTATE_NO ) {
        parseLoop();
      }
      if ( getState() == ISTATE_NO ) {
        // TODO KDE4: pass cmd->resultInfo() as third argument.
        // ERR_CONNECTION_BROKEN expects a host, no way to pass details about the problem.
        error( ERR_CONNECTION_BROKEN, myHost );
        completeQueue.removeAll( cmd );
        closeConnection();
        return;
      } else if ( cmd->result() != "OK" ) {
        error( ERR_SLAVE_DEFINED, cmd->resultInfo() );
        completeQueue.removeAll( cmd );
        return;
      } else {
        if ( hasCapability( "UIDPLUS" ) ) {
          QString uid = cmd->resultInfo();
          if ( uid.contains( "APPENDUID" ) ) {
            uid = uid.section( ' ', 2, 2 );
            uid.truncate( uid.length() - 1 );
            infoMessage( "UID " + uid );
          }
        }
        // MUST reselect to get the new message
        else if ( aBox == getCurrentBox() ) {
          cmd =
            doCommand( imapCommand::clientSelect( aBox, !selectInfo.readWrite() ) );
          completeQueue.removeAll( cmd );
        }
      }
    } else {
      //error(ERR_COULD_NOT_WRITE, myHost);
      // Better ship the error message, e.g. "Over Quota"
      error( ERR_SLAVE_DEFINED, cmd->resultInfo() );
      completeQueue.removeAll( cmd );
      return;
    }

    completeQueue.removeAll( cmd );
  }

  finished();
}

void
IMAP4Protocol::mkdir (const KUrl & _url, int)
{
  kDebug( 7116 ) << "IMAP4::mkdir -" << _url.prettyUrl();
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
  kDebug( 7116 ) << "IMAP4::mkdir - create" << aBox;
  CommandPtr cmd = doCommand( imapCommand::clientCreate( aBox ) );

  if ( cmd->result() != "OK" ) {
    kDebug( 7116 ) << "IMAP4::mkdir -" << cmd->resultInfo();
    error( ERR_COULD_NOT_MKDIR, _url.prettyUrl() );
    completeQueue.removeAll( cmd );
    return;
  }
  completeQueue.removeAll( cmd );

  // start a new listing to find the type of the folder
  enum IMAP_TYPE type =
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
  if ( type == ITYPE_BOX ) {
    bool ask = ( aInfo.contains( "ASKUSER" ) );
    if ( ask &&
         messageBox( QuestionYesNo,
           i18n( "The following folder will be created on the server: %1 "
                 "What do you want to store in this folder?", aBox ),
           i18n( "Create Folder" ),
           i18n( "&Messages" ), i18n( "&Subfolders" ) ) == KMessageBox::No ) {
      cmd = doCommand( imapCommand::clientDelete( aBox ) );
      completeQueue.removeAll( cmd );
      cmd = doCommand( imapCommand::clientCreate( aBox + aDelimiter ) );
      if ( cmd->result() != "OK" ) {
        error( ERR_COULD_NOT_MKDIR, _url.prettyUrl() );
        completeQueue.removeAll( cmd );
        return;
      }
      completeQueue.removeAll( cmd );
    }
  }

  cmd = doCommand( imapCommand::clientSubscribe( aBox ) );
  completeQueue.removeAll( cmd );

  finished();
}

void
IMAP4Protocol::copy (const KUrl & src, const KUrl & dest, int, KIO::JobFlags flags)
{
  kDebug( 7116 ) << "IMAP4::copy - [" << ( ( flags & KIO::Overwrite ) ? "Overwrite" : "NoOverwrite" ) << "]" << src.prettyUrl() << " ->" << dest.prettyUrl();
  QString sBox, sSequence, sLType, sSection, sValidity, sDelimiter, sInfo;
  QString dBox, dSequence, dLType, dSection, dValidity, dDelimiter, dInfo;
  enum IMAP_TYPE sType =
    parseURL( src, sBox, sSection, sLType, sSequence, sValidity, sDelimiter, sInfo );
  enum IMAP_TYPE dType =
    parseURL( dest, dBox, dSection, dLType, dSequence, dValidity, dDelimiter, dInfo );

  // see if we have to create anything
  if ( dType != ITYPE_BOX && dType != ITYPE_DIR_AND_BOX ) {
    // this might be konqueror
    int sub = dBox.indexOf( sBox );

    // might be moving to upper folder
    if ( sub > 0 ) {
      KUrl testDir = dest;

      QString subDir = dBox.right( dBox.length() - dBox.lastIndexOf( '/' ) );
      QString topDir = dBox.left( sub );
      testDir.setPath( '/' + topDir );
      dType =
        parseURL( testDir, topDir, dSection, dLType, dSequence, dValidity, dDelimiter, dInfo );

      kDebug( 7116 ) << "IMAP4::copy - checking this destination" << topDir;
      // see if this is what the user wants
      if ( dType == ITYPE_BOX || dType == ITYPE_DIR_AND_BOX ) {
        kDebug( 7116 ) << "IMAP4::copy - assuming this destination" << topDir;
        dBox = topDir;
      } else {

        // maybe if we create a new mailbox
        topDir = '/' + topDir + subDir;
        testDir.setPath( topDir );
        kDebug( 7116 ) << "IMAP4::copy - checking this destination" << topDir;
        dType =
          parseURL( testDir, topDir, dSection, dLType, dSequence, dValidity, dDelimiter, dInfo );
        if ( dType != ITYPE_BOX && dType != ITYPE_DIR_AND_BOX ) {
          // ok then we'll create a mailbox
          CommandPtr cmd = doCommand( imapCommand::clientCreate( topDir ) );

          // on success we'll use it, else we'll just try to create the given dir
          if ( cmd->result() == "OK" ) {
            kDebug( 7116 ) << "IMAP4::copy - assuming this destination" << topDir;
            dType = ITYPE_BOX;
            dBox = topDir;
          } else {
            completeQueue.removeAll( cmd );
            cmd = doCommand( imapCommand::clientCreate( dBox ) );
            if ( cmd->result() == "OK" ) {
              dType = ITYPE_BOX;
            } else {
              error( ERR_COULD_NOT_WRITE, dest.prettyUrl() );
            }
          }
          completeQueue.removeAll( cmd );
        }
      }

    }
  }
  if ( sType == ITYPE_MSG || sType == ITYPE_BOX || sType == ITYPE_DIR_AND_BOX ) {
    //select the source box
    if ( !assureBox( sBox, true ) ) {
      return;
    }
    kDebug( 7116 ) << "IMAP4::copy -" << sBox << " ->" << dBox;

    //issue copy command
    CommandPtr cmd =
      doCommand( imapCommand::clientCopy( dBox, sSequence ) );
    if ( cmd->result() != "OK" ) {
      kError( 5006 ) << "IMAP4::copy -" << cmd->resultInfo();
      error( ERR_COULD_NOT_WRITE, dest.prettyUrl() );
      completeQueue.removeAll( cmd );
      return;
    } else {
      if ( hasCapability( "UIDPLUS" ) ) {
        QString uid = cmd->resultInfo();
        if ( uid.contains( "COPYUID" ) ) {
          uid = uid.section( ' ', 2, 3 );
          uid.truncate( uid.length() - 1 );
          infoMessage( "UID " + uid );
        }
      }
    }
    completeQueue.removeAll( cmd );
  } else {
    error( ERR_ACCESS_DENIED, src.prettyUrl() );
    return;
  }
  finished();
}

void
IMAP4Protocol::del (const KUrl & _url, bool isFile)
{
  kDebug( 7116 ) << "IMAP4::del - [" << ( isFile ? "File" : "NoFile" ) << "]" << _url.prettyUrl();
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  enum IMAP_TYPE aType =
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );

  switch ( aType ) {
  case ITYPE_BOX:
  case ITYPE_DIR_AND_BOX:
    if ( !aSequence.isEmpty() ) {
      if ( aSequence == "*" ) {
        if ( !assureBox( aBox, false ) ) {
          return;
        }
        CommandPtr cmd = doCommand( imapCommand::clientExpunge() );
        if ( cmd->result() != "OK" ) {
          error( ERR_CANNOT_DELETE, _url.prettyUrl() );
          completeQueue.removeAll( cmd );
          return;
        }
        completeQueue.removeAll( cmd );
      } else {
        // if open for read/write
        if ( !assureBox( aBox, false ) ) {
          return;
        }
        CommandPtr cmd =
          doCommand( imapCommand::clientStore( aSequence, "+FLAGS.SILENT", "\\DELETED" ) );
        if ( cmd->result() != "OK" ) {
          error( ERR_CANNOT_DELETE, _url.prettyUrl() );
          completeQueue.removeAll( cmd );
          return;
        }
        completeQueue.removeAll( cmd );
      }
    } else {
      if ( getCurrentBox() == aBox ) {
        CommandPtr cmd = doCommand( imapCommand::clientClose() );
        completeQueue.removeAll( cmd );
        setState( ISTATE_LOGIN );
      }
      // We unsubscribe, otherwise we get ghost folders on UW-IMAP
      CommandPtr cmd = doCommand( imapCommand::clientUnsubscribe( aBox ) );
      completeQueue.removeAll( cmd );
      cmd = doCommand( imapCommand::clientDelete( aBox ) );
      // If this doesn't work, we try to empty the mailbox first
      if ( cmd->result() != "OK" ) {
        completeQueue.removeAll( cmd );
        if ( !assureBox( aBox, false ) ) {
          return;
        }
        bool stillOk = true;
        if ( stillOk ) {
          CommandPtr cmd = doCommand( imapCommand::clientStore( "1:*", "+FLAGS.SILENT", "\\DELETED" ) );
          if ( cmd->result() != "OK" ) {
            stillOk = false;
          }
          completeQueue.removeAll( cmd );
        }
        if ( stillOk ) {
          CommandPtr cmd = doCommand( imapCommand::clientClose() );
          if ( cmd->result() != "OK" ) {
            stillOk = false;
          }
          completeQueue.removeAll( cmd );
          setState( ISTATE_LOGIN );
        }
        if ( stillOk ) {
          CommandPtr cmd = doCommand( imapCommand::clientDelete( aBox ) );
          if ( cmd->result() != "OK" ) {
            stillOk = false;
          }
          completeQueue.removeAll( cmd );
        }
        if ( !stillOk ) {
          error( ERR_COULD_NOT_RMDIR, _url.prettyUrl() );
          return;
        }
      } else {
        completeQueue.removeAll( cmd );
      }
    }
    break;

  case ITYPE_DIR:
    {
      CommandPtr cmd = doCommand( imapCommand::clientDelete( aBox ) );
      if ( cmd->result() != "OK" ) {
        error( ERR_COULD_NOT_RMDIR, _url.prettyUrl() );
        completeQueue.removeAll( cmd );
        return;
      }
      completeQueue.removeAll( cmd );
    }
    break;

  case ITYPE_MSG:
    {
      // if open for read/write
      if ( !assureBox ( aBox, false ) ) {
        return;
      }
      CommandPtr cmd =
        doCommand( imapCommand::clientStore( aSequence, "+FLAGS.SILENT", "\\DELETED" ) );
      if ( cmd->result() != "OK" ) {
        error( ERR_CANNOT_DELETE, _url.prettyUrl() );
        completeQueue.removeAll( cmd );
        return;
      }
      completeQueue.removeAll( cmd );
    }
    break;

  case ITYPE_UNKNOWN:
  case ITYPE_ATTACH:
    error( ERR_CANNOT_DELETE, _url.prettyUrl() );
    break;
  }
  finished();
}

/*
 * Copy a mail: data = 'C' + srcURL (KUrl) + destURL (KUrl)
 * Capabilities: data = 'c'. Result shipped in infoMessage() signal
 * No-op: data = 'N'
 * Namespace: data = 'n'. Result shipped in infoMessage() signal
 *                        The format is: section=namespace=delimiter
 *                        Note that the namespace can be empty
 * Unsubscribe: data = 'U' + URL (KUrl)
 * Subscribe: data = 'u' + URL (KUrl)
 * Change the status: data = 'S' + URL (KUrl) + Flags (QCString)
 * ACL commands: data = 'A' + command + URL (KUrl) + command-dependent args
 * AnnotateMore commands: data = 'M' + 'G'et/'S'et + URL + entry + command-dependent args
 * Search: data = 'E' + URL (KUrl)
 * Quota commands: data = 'Q' + 'R'oot/'G'et/'S'et + URL + entry + command-dependent args
 * Custom command: data = 'X' + 'N'ormal/'E'xtended + command + command-dependent args
 */
void
IMAP4Protocol::special (const QByteArray & aData)
{
  kDebug( 7116 ) << "IMAP4Protocol::special";
  if ( !makeLogin() ) {
    return;
  }

  QDataStream stream( aData );

  int tmp;
  stream >> tmp;

  switch ( tmp ) {
  case 'C':
  {
    // copy
    KUrl src;
    KUrl dest;
    stream >> src >> dest;
    copy( src, dest, 0, KIO::DefaultFlags );
    break;
  }
  case 'c':
  {
    // capabilities
    infoMessage( imapCapabilities.join( " " ) );
    finished();
    break;
  }
  case 'N':
  {
    // NOOP
    CommandPtr cmd = doCommand( imapCommand::clientNoop() );
    if ( cmd->result() != "OK" ) {
      kDebug( 7116 ) << "NOOP did not succeed - connection broken";
      completeQueue.removeAll( cmd );
      error( ERR_CONNECTION_BROKEN, myHost );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'n':
  {
    // namespace in the form "section=namespace=delimiter"
    // entries are separated by ,
    infoMessage( imapNamespaces.join( "," ) );
    finished();
    break;
  }
  case 'U':
  {
    // unsubscribe
    KUrl _url;
    stream >> _url;
    QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
    CommandPtr cmd = doCommand( imapCommand::clientUnsubscribe( aBox ) );
    if ( cmd->result() != "OK" ) {
      completeQueue.removeAll( cmd );
      error( ERR_SLAVE_DEFINED, i18n( "Unsubscribe of folder %1 "
                                      "failed. The server returned: %2",
                                      _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'u':
  {
    // subscribe
    KUrl _url;
    stream >> _url;
    QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
    CommandPtr cmd = doCommand( imapCommand::clientSubscribe( aBox ) );
    if ( cmd->result() != "OK" ) {
      completeQueue.removeAll( cmd );
      error( ERR_SLAVE_DEFINED, i18n( "Subscribe of folder %1 "
                                      "failed. The server returned: %2",
                                      _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'A':
  {
    // acl
    int cmd;
    stream >> cmd;
    if ( hasCapability( "ACL" ) ) {
      specialACLCommand( cmd, stream );
    } else {
      error( ERR_UNSUPPORTED_ACTION, QString::fromLatin1( "ACL" ) );
    }
    break;
  }
  case 'M':
  {
    // annotatemore
    int cmd;
    stream >> cmd;
    if ( hasCapability( "ANNOTATEMORE" ) ) {
      specialAnnotateMoreCommand( cmd, stream );
    } else {
      error( ERR_UNSUPPORTED_ACTION, QString::fromLatin1( "ANNOTATEMORE" ) );
    }
    break;
  }
  case 'Q':
  {
    // quota
    int cmd;
    stream >> cmd;
    if ( hasCapability( "QUOTA" ) ) {
      specialQuotaCommand( cmd, stream );
    } else {
      error( ERR_UNSUPPORTED_ACTION, QString::fromLatin1( "QUOTA" ) );
    }
    break;
  }
  case 'S':
  {
    // status
    KUrl _url;
    QByteArray newFlags;
    stream >> _url >> newFlags;

    QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
    if ( !assureBox( aBox, false ) ) {
      return;
    }

    // make sure we only touch flags we know
    QByteArray knownFlags = "\\SEEN \\ANSWERED \\FLAGGED \\DRAFT";
    const imapInfo info = getSelected();
    if ( info.permanentFlagsAvailable() && ( info.permanentFlags() & imapInfo::User ) ) {
      knownFlags += " KMAILFORWARDED KMAILTODO KMAILWATCHED KMAILIGNORED $FORWARDED $TODO $WATCHED $IGNORED";
    }

    CommandPtr cmd = doCommand( imapCommand::clientStore( aSequence, "-FLAGS.SILENT", knownFlags ) );
    if ( cmd->result() != "OK" ) {
      completeQueue.removeAll( cmd );
      error( ERR_SLAVE_DEFINED, i18n( "Changing the flags of message %1 "
                                      "failed with %2.",
                                      _url.prettyUrl(), cmd->result() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    if ( !newFlags.isEmpty() ) {
      cmd = doCommand( imapCommand::clientStore( aSequence, "+FLAGS.SILENT", newFlags ) );
      if ( cmd->result() != "OK" ) {
        completeQueue.removeAll( cmd );
        error( ERR_SLAVE_DEFINED, i18n( "Silent Changing the flags of message %1 "
                                        "failed with %2.",
                                        _url.prettyUrl(), cmd->result() ) );
        return;
      }
      completeQueue.removeAll( cmd );
    }
    finished();
    break;
  }
  case 's':
  {
    // seen
    KUrl _url;
    bool seen;
    QByteArray newFlags;
    stream >> _url >> seen;

    QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
    if ( !assureBox( aBox, true ) ) { // read-only because changing SEEN should be possible even then
      return;
    }

    CommandPtr cmd;
    if ( seen ) {
      cmd = doCommand( imapCommand::clientStore( aSequence, "+FLAGS.SILENT", "\\SEEN" ) );
    } else {
      cmd = doCommand( imapCommand::clientStore( aSequence, "-FLAGS.SILENT", "\\SEEN" ) );
    }

    if ( cmd->result() != "OK" ) {
      completeQueue.removeAll( cmd );
      error( ERR_SLAVE_DEFINED, i18n( "Changing the flags of message %1 failed.",
                                      _url.prettyUrl() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }

  case 'E':
  {
    // search
    specialSearchCommand( stream );
    break;
  }
  case 'X':
  {
    // custom command
    specialCustomCommand( stream );
    break;
  }
  default:
    kWarning( 7116 ) << "Unknown command in special():" << tmp;
    error( ERR_UNSUPPORTED_ACTION, QString( QChar( tmp ) ) );
    break;
  }
}

void
IMAP4Protocol::specialACLCommand( int command, QDataStream& stream )
{
  // All commands start with the URL to the box
  KUrl _url;
  stream >> _url;
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );

  switch ( command ) {
  case 'S': // SETACL
  {
    QString user, acl;
    stream >> user >> acl;
    kDebug( 7116 ) << "SETACL" << aBox << user << acl;
    CommandPtr cmd = doCommand( imapCommand::clientSetACL( aBox, user, acl ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Setting the Access Control List on folder %1 "
                                       "for user %2 failed. The server returned: %3",
                                       _url.prettyUrl(), user, cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'D': // DELETEACL
  {
    QString user;
    stream >> user;
    kDebug( 7116 ) << "DELETEACL" << aBox << user;
    CommandPtr cmd = doCommand( imapCommand::clientDeleteACL( aBox, user ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Deleting the Access Control List on folder %1 "
                                      "for user %2 failed. The server returned: %3",
                                       _url.prettyUrl(), user, cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'G': // GETACL
  {
    kDebug( 7116 ) << "GETACL" << aBox;
    CommandPtr cmd = doCommand( imapCommand::clientGetACL( aBox ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Retrieving the Access Control List on folder %1 "
                                      "failed. The server returned: %2",
                                      _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    // Returning information to the application from a special() command isn't easy.
    // I'm reusing the infoMessage trick seen above (for capabilities), but this
    // limits me to a string instead of a stringlist. Using DQUOTE as separator,
    // because it's forbidden in userids by rfc3501
    kDebug( 7116 ) << getResults();
    infoMessage( getResults().join( "\"" ) );
    finished();
    break;
  }
  case 'L': // LISTRIGHTS
  {
    // Do we need this one? It basically shows which rights are tied together, but that's all?
    error( ERR_UNSUPPORTED_ACTION, QString( QChar( command ) ) );
    break;
  }
  case 'M': // MYRIGHTS
  {
    kDebug( 7116 ) << "MYRIGHTS" << aBox;
    CommandPtr cmd = doCommand( imapCommand::clientMyRights( aBox ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Retrieving the Access Control List on folder %1 "
                                      "failed. The server returned: %2",
                                      _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    QStringList lst = getResults();
    kDebug( 7116 ) << "myrights results:" << lst;
    if ( !lst.isEmpty() ) {
      Q_ASSERT( lst.count() == 1 );
      infoMessage( lst.first() );
    }
    finished();
    break;
  }
  default:
    kWarning( 7116 ) << "Unknown special ACL command:" << command;
    error( ERR_UNSUPPORTED_ACTION, QString( QChar( command ) ) );
  }
}

void
IMAP4Protocol::specialSearchCommand( QDataStream& stream )
{
  kDebug( 7116 ) << "IMAP4Protocol::specialSearchCommand";
  KUrl _url;
  stream >> _url;
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );
  if ( !assureBox( aBox, true ) ) {
    return;
  }

  CommandPtr cmd = doCommand( imapCommand::clientSearch( aSection ) );
  if ( cmd->result() != "OK" ) {
    error( ERR_SLAVE_DEFINED, i18n( "Searching of folder %1 "
                                    "failed. The server returned: %2",
                                    aBox, cmd->resultInfo() ) );
    return;
  }
  completeQueue.removeAll( cmd );
  QStringList lst = getResults();
  kDebug( 7116 ) << "IMAP4Protocol::specialSearchCommand '" << aSection
                 << "' returns" << lst;
  infoMessage( lst.join( " " ) );

  finished();
}

void
IMAP4Protocol::specialCustomCommand( QDataStream& stream )
{
  kDebug( 7116 ) << "IMAP4Protocol::specialCustomCommand" << endl;

  QString command, arguments;
  int type;
  stream >> type;
  stream >> command >> arguments;

  /**
   * In 'normal' mode we send the command with all information in one go
   * and retrieve the result.
   */
  if ( type == 'N' ) {
    kDebug( 7116 ) << "IMAP4Protocol::specialCustomCommand: normal mode" << endl;
    CommandPtr cmd = doCommand( imapCommand::clientCustom( command, arguments ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Custom command %1:%2 failed. The server returned: %3",
                                      command, arguments, cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    QStringList lst = getResults();
    kDebug( 7116 ) << "IMAP4Protocol::specialCustomCommand '" << command
                   << ":" << arguments
                   << "' returns " << lst << endl;
    infoMessage( lst.join( " " ) );

    finished();
  } else
  /**
   * In 'extended' mode we send a first header and push the data of the request in
   * streaming mode.
   */
  if ( type == 'E' ) {
    kDebug( 7116 ) << "IMAP4Protocol::specialCustomCommand: extended mode" << endl;
    CommandPtr cmd = sendCommand( imapCommand::clientCustom( command, QString() ) );
    while ( !parseLoop() ) {
    };

    // see if server is waiting
    if ( !cmd->isComplete() && !getContinuation().isEmpty() ) {
      const QByteArray buffer = arguments.toUtf8();

      // send data to server
      bool sendOk = ( write( buffer.data(), buffer.size() ) == (ssize_t)buffer.size() );
      processedSize( buffer.size() );

      if ( !sendOk ) {
        error( ERR_CONNECTION_BROKEN, myHost );
        completeQueue.removeAll( cmd );
        setState( ISTATE_CONNECT );
        closeConnection();
        return;
      }
    }
    parseWriteLine( "" );

    do {
      while ( !parseLoop() ) {
      };
    } while ( !cmd->isComplete() );

    completeQueue.removeAll( cmd );

    QStringList lst = getResults();
    kDebug( 7116 ) << "IMAP4Protocol::specialCustomCommand: returns " << lst << endl;
    infoMessage( lst.join( " " ) );

    finished();
  }
}

void
IMAP4Protocol::specialAnnotateMoreCommand( int command, QDataStream& stream )
{
  // All commands start with the URL to the box
  KUrl _url;
  stream >> _url;
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );

  switch ( command ) {
  case 'S': // SETANNOTATION
  {
    // Params:
    //  KUrl URL of the mailbox
    //  QString entry (should be an actual entry name, no % or *; empty for server entries)
    //  QMap<QString,QString> attributes (name and value)
    QString entry;
    QMap<QString, QString> attributes;
    stream >> entry >> attributes;
    kDebug( 7116 ) << "SETANNOTATION" << aBox << entry << attributes.count() << " attributes";
    CommandPtr cmd = doCommand( imapCommand::clientSetAnnotation( aBox, entry, attributes ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Setting the annotation %1 on folder %2 "
                                      "failed. The server returned: %3",
                                      entry, _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    completeQueue.removeAll( cmd );
    finished();
    break;
  }
  case 'G': // GETANNOTATION.
  {
    // Params:
    //  KUrl URL of the mailbox
    //  QString entry (should be an actual entry name, no % or *; empty for server entries)
    //  QStringList attributes (list of attributes to be retrieved, possibly with % or *)
    QString entry;
    QStringList attributeNames;
    stream >> entry >> attributeNames;
    kDebug( 7116 ) << "GETANNOTATION" << aBox << entry << attributeNames;
    CommandPtr cmd = doCommand( imapCommand::clientGetAnnotation( aBox, entry, attributeNames ) );
    if ( cmd->result() != "OK" ) {
      error( ERR_SLAVE_DEFINED, i18n( "Retrieving the annotation %1 on folder %2 "
                                      "failed. The server returned: %3",
                                      entry, _url.prettyUrl(), cmd->resultInfo() ) );
      return;
    }
    // Returning information to the application from a special() command isn't easy.
    // I'm reusing the infoMessage trick seen above (for capabilities and acls), but this
    // limits me to a string instead of a stringlist. Let's use \r as separator.
    kDebug( 7116 ) << getResults();
    infoMessage( getResults().join( "\r" ) );
    finished();
    break;
  }
  default:
    kWarning( 7116 ) << "Unknown special annotate command:" << command;
    error( ERR_UNSUPPORTED_ACTION, QString( QChar( command ) ) );
  }
}

void
IMAP4Protocol::specialQuotaCommand( int command, QDataStream& stream )
{
  // All commands start with the URL to the box
  KUrl _url;
  stream >> _url;
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo );

  switch ( command ) {
    case 'R': // GETQUOTAROOT
      {
        kDebug( 7116 ) << "QUOTAROOT" << aBox;
        CommandPtr cmd = doCommand( imapCommand::clientGetQuotaroot( aBox ) );
        if ( cmd->result() != "OK" ) {
          error( ERR_SLAVE_DEFINED, i18n( "Retrieving the quota root information on folder %1 "
                                          "failed. The server returned: %2",
                                          _url.prettyUrl(), cmd->resultInfo() ) );
          return;
        }
        infoMessage( getResults().join( "\r" ) );
        finished();
        break;
      }
    case 'G': // GETQUOTA
      {
        kDebug( 7116 ) << "GETQUOTA command";
        kWarning( 7116 ) << "UNIMPLEMENTED";
        break;
      }
    case 'S': // SETQUOTA
      {
        kDebug( 7116 ) << "SETQUOTA command";
        kWarning( 7116 ) << "UNIMPLEMENTED";
        break;
      }
    default:
      kWarning( 7116 ) << "Unknown special quota command:" << command;
      error( ERR_UNSUPPORTED_ACTION, QString( QChar( command ) ) );
  }
}

void
IMAP4Protocol::rename (const KUrl & src, const KUrl & dest, KIO::JobFlags flags)
{
  kDebug( 7116 ) << "IMAP4::rename - [" << ( ( flags & KIO::Overwrite ) ? "Overwrite" : "NoOverwrite" ) << "]" << src << " ->" << dest;
  QString sBox, sSequence, sLType, sSection, sValidity, sDelimiter, sInfo;
  QString dBox, dSequence, dLType, dSection, dValidity, dDelimiter, dInfo;
  enum IMAP_TYPE sType =
    parseURL( src, sBox, sSection, sLType, sSequence, sValidity, sDelimiter, sInfo, false );
  enum IMAP_TYPE dType =
    parseURL( dest, dBox, dSection, dLType, dSequence, dValidity, dDelimiter, dInfo, false );

  if ( dType == ITYPE_UNKNOWN ) {
    switch ( sType ) {
    case ITYPE_BOX:
    case ITYPE_DIR:
    case ITYPE_DIR_AND_BOX:
      {
        if ( getState() == ISTATE_SELECT && sBox == getCurrentBox() ) {
          kDebug( 7116 ) << "IMAP4::rename - close" << getCurrentBox();
          // mailbox can only be renamed if it is closed
          CommandPtr cmd = doCommand( imapCommand::clientClose() );
          bool ok = cmd->result() == "OK";
          completeQueue.removeAll( cmd );
          if ( !ok ) {
            error( ERR_SLAVE_DEFINED, i18n( "Unable to close mailbox." ) );
            return;
          }
          setState( ISTATE_LOGIN );
        }
        CommandPtr cmd = doCommand( imapCommand::clientRename( sBox, dBox ) );
        if ( cmd->result() != "OK" ) {
          error( ERR_CANNOT_RENAME, cmd->result() );
          completeQueue.removeAll( cmd );
          return;
        }
        completeQueue.removeAll( cmd );
      }
      break;

    case ITYPE_MSG:
    case ITYPE_ATTACH:
    case ITYPE_UNKNOWN:
      error( ERR_CANNOT_RENAME, src.prettyUrl() );
      break;
    }
  } else {
    error( ERR_CANNOT_RENAME, src.prettyUrl() );
    return;
  }
  finished();
}

void
IMAP4Protocol::slave_status()
{
  bool connected = (getState() != ISTATE_NO) && isConnected();
  kDebug( 7116 ) << "IMAP4::slave_status" << connected;
  slaveStatus( connected ? myHost : QString(), connected );
}

void
IMAP4Protocol::dispatch (int command, const QByteArray & data)
{
  kDebug( 7116 ) << "IMAP4::dispatch - command=" << command;
  KIO::TCPSlaveBase::dispatch( command, data );
}

void
IMAP4Protocol::stat (const KUrl & _url)
{
  kDebug( 7116 ) << "IMAP4::stat -" << _url.prettyUrl();
  QString aBox, aSequence, aLType, aSection, aValidity, aDelimiter, aInfo;
  // parseURL with caching
  enum IMAP_TYPE aType =
    parseURL( _url, aBox, aSection, aLType, aSequence, aValidity, aDelimiter, aInfo, true );

  UDSEntry entry;

  entry.insert( UDSEntry::UDS_NAME, aBox );

  if ( !aSection.isEmpty() ) {
    if ( getState() == ISTATE_SELECT && aBox == getCurrentBox() ) {
      CommandPtr cmd = doCommand( imapCommand::clientClose() );
      bool ok = cmd->result() == "OK";
      completeQueue.removeAll( cmd );
      if ( !ok ) {
        error( ERR_SLAVE_DEFINED, i18n( "Unable to close mailbox." ) );
        return;
      }
      setState( ISTATE_LOGIN );
    }
    bool ok = false;
    QString cmdInfo;
    if ( aType == ITYPE_MSG || aType == ITYPE_ATTACH ) {
      ok = true;
    } else {
      CommandPtr cmd = doCommand( imapCommand::clientStatus( aBox, aSection ) );
      ok = cmd->result() == "OK";
      cmdInfo = cmd->resultInfo();
      completeQueue.removeAll( cmd );
    }
    if ( !ok ) {
      bool found = false;
      CommandPtr cmd = doCommand( imapCommand::clientList( "", aBox ) );
      if ( cmd->result() == "OK" ) {
        for ( QList< imapList >::Iterator it = listResponses.begin();
             it != listResponses.end(); ++it ) {
          if ( aBox == ( *it ).name() ) {
            found = true;
          }
        }
      }
      completeQueue.removeAll( cmd );
      if ( found ) {
        error( ERR_SLAVE_DEFINED, i18n( "Unable to get information about folder %1. The server replied: %2",
                                        aBox, cmdInfo ) );
      } else {
        error( KIO::ERR_DOES_NOT_EXIST, aBox );
      }
      return;
    }
    if ( ( aSection == "UIDNEXT" && getStatus().uidNextAvailable() ) ||
         ( aSection == "UNSEEN" && getStatus().unseenAvailable() ) ) {
      entry.insert( UDSEntry::UDS_SIZE, ( aSection == "UIDNEXT" ) ? getStatus().uidNext()
                                        : getStatus().unseen() );
    }
  } else if ( aType == ITYPE_BOX ||
              aType == ITYPE_DIR_AND_BOX ||
              aType == ITYPE_MSG ||
              aType == ITYPE_ATTACH ) {
    ulong validity = 0;
    // see if the box is already in select/examine state
    if ( aBox == getCurrentBox() ) {
      validity = selectInfo.uidValidity();
    } else {
      // do a status lookup on the box
      // only do this if the box is not selected
      // the server might change the validity for new select/examine
      CommandPtr cmd = doCommand( imapCommand::clientStatus( aBox, "UIDVALIDITY" ) );
      completeQueue.removeAll( cmd );
      validity = getStatus().uidValidity();
    }
#ifdef __GNUC__
#warning This is temporary since Dec 2000 and makes most of the below code invalid
#endif
    validity = 0;               // temporary

    if ( aType == ITYPE_BOX || aType == ITYPE_DIR_AND_BOX ) {
      // has no or an invalid uidvalidity
      if ( validity > 0 && validity != aValidity.toULong() ) {
        //redirect
        KUrl newUrl = _url;

        newUrl.setPath( '/' + aBox + ";UIDVALIDITY=" +
                        QString::number( validity ) );
        kDebug( 7116 ) << "IMAP4::stat - redirecting to" << newUrl.prettyUrl();
        redirection( newUrl );
      }
    } else if ( aType == ITYPE_MSG || aType == ITYPE_ATTACH ) {
      //must determine if this message exists
      //cause konqueror will check this on paste operations

      // has an invalid uidvalidity
      // or no messages in box
      if ( validity > 0 && validity != aValidity.toULong() ) {
        aType = ITYPE_UNKNOWN;
        kDebug( 7116 ) << "IMAP4::stat - url has invalid validity [" << validity << "d]" << _url.prettyUrl();
      }
    }
  }

  entry.insert( UDSEntry::UDS_MIME_TYPE, getMimeType( aType ) );

  //kDebug( 7116 ) << "IMAP4: stat:" << atom.m_str;
  switch ( aType ) {
  case ITYPE_DIR:
    entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFDIR );
    break;

  case ITYPE_BOX:
  case ITYPE_DIR_AND_BOX:
    entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFDIR );
    break;

  case ITYPE_MSG:
  case ITYPE_ATTACH:
    entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFREG );
    break;

  case ITYPE_UNKNOWN:
    error( ERR_DOES_NOT_EXIST, _url.prettyUrl() );
    break;
  }

  statEntry( entry );
  kDebug( 7116 ) << "IMAP4::stat - Finishing stat";
  finished();
}

void IMAP4Protocol::openConnection()
{
  if ( makeLogin() ) {
    connected();
  }
}

void IMAP4Protocol::closeConnection()
{
  if ( getState() == ISTATE_NO ) {
    return;
  }
  if ( getState() == ISTATE_SELECT && metaData( "expunge" ) == "auto" ) {
    CommandPtr cmd = doCommand( imapCommand::clientExpunge() );
    completeQueue.removeAll( cmd );
  }
  if ( getState() != ISTATE_CONNECT ) {
    CommandPtr cmd = doCommand( imapCommand::clientLogout() );
    completeQueue.removeAll( cmd );
  }
  disconnectFromHost();
  setState( ISTATE_NO );
  completeQueue.clear();
  sentQueue.clear();
  lastHandled = 0;
  currentBox.clear();
  readBufferLen = 0;
}

bool IMAP4Protocol::makeLogin()
{
  if ( getState() == ISTATE_LOGIN || getState() == ISTATE_SELECT ) {
    return true;
  }

  kDebug( 7116 ) << "IMAP4::makeLogin - checking login";
  bool alreadyConnected = getState() == ISTATE_CONNECT;
  kDebug( 7116 ) << "IMAP4::makeLogin - alreadyConnected" << alreadyConnected;
  if ( alreadyConnected ||
       connectToHost( ( mySSL ? IMAP_SSL_PROTOCOL : IMAP_PROTOCOL ), myHost, myPort ) ) {
//      fcntl(m_iSock, F_SETFL, (fcntl(m_iSock, F_GETFL) | O_NDELAY));

    setState( ISTATE_CONNECT );

    myAuth = metaData( "auth" );
    myTLS  = metaData( "tls" );
    kDebug( 7116 ) << "myAuth:" << myAuth;

    CommandPtr cmd;

    unhandled.clear();
    if ( !alreadyConnected ) {
      while ( !parseLoop() ) {
      }   //get greeting
    }
    QString greeting;
    if ( !unhandled.isEmpty() ) {
      greeting = unhandled.first().trimmed();
    }
    unhandled.clear();       //get rid of it
    cmd = doCommand( CommandPtr( new imapCommand( "CAPABILITY", "" ) ) );

    kDebug( 7116 ) << "IMAP4: setHost: capability";
    for ( QStringList::const_iterator it = imapCapabilities.constBegin();
         it != imapCapabilities.constEnd(); ++it ) {
      kDebug( 7116 ) << "'" << ( *it ) << "'";
    }
    completeQueue.removeAll( cmd );

    if ( !hasCapability( "IMAP4" ) && !hasCapability( "IMAP4rev1" ) ) {
      error( ERR_COULD_NOT_LOGIN, i18n( "The server %1 supports neither "
                                        "IMAP4 nor IMAP4rev1.\nIt identified itself with: %2",
                                        myHost, greeting ) );
      closeConnection();
      return false;
    }

    if ( metaData( "nologin" ) == "on" ) {
      return true;
    }

    if ( myTLS == "on" && !hasCapability( QString( "STARTTLS" ) ) ) {
      error( ERR_COULD_NOT_LOGIN, i18n( "The server does not support TLS.\n"
                                        "Disable this security feature to connect unencrypted." ) );
      closeConnection();
      return false;
    }
    if ( ( myTLS == "on" /*###|| ( canUseTLS() && myTLS != "off")*/ ) &&
         hasCapability( QString( "STARTTLS" ) ) ) {
      CommandPtr cmd = doCommand( imapCommand::clientStartTLS() );
      if ( cmd->result() == "OK" ) {
        completeQueue.removeAll( cmd );
        if ( startSsl() ) {
          kDebug( 7116 ) << "TLS mode has been enabled.";
          CommandPtr cmd2 = doCommand( CommandPtr( new imapCommand( "CAPABILITY", "" ) ) );
          for ( QStringList::const_iterator it = imapCapabilities.constBegin();
                it != imapCapabilities.constEnd(); ++it ) {
            kDebug( 7116 ) << "'" << ( *it ) << "'";
          }
          completeQueue.removeAll( cmd2 );
        } else {
          kWarning( 7116 ) << "TLS mode setup has failed.  Aborting.";
          error( ERR_COULD_NOT_LOGIN, i18n( "Starting TLS failed." ) );
          closeConnection();
          return false;
        }
      } else {
        completeQueue.removeAll( cmd );
      }
    }

    if ( !myAuth.isEmpty() && myAuth != "*" &&
         !hasCapability( QString( "AUTH=" ) + myAuth ) ) {
      error( ERR_COULD_NOT_LOGIN, i18n( "The authentication method %1 is not "
                                        "supported by the server.", myAuth ) );
      closeConnection();
      return false;
    }

    if (  greeting.contains(  QRegExp(  "Cyrus IMAP4 v2.1" ) ) ) {
      removeCapability( "ANNOTATEMORE" );
    }

    // starting from Cyrus IMAP 2.3.9, shared seen flags are available
    QRegExp regExp( "Cyrus\\sIMAP[4]{0,1}\\sv(\\d+)\\.(\\d+)\\.(\\d+)", Qt::CaseInsensitive );
    if ( regExp.indexIn( greeting ) >= 0 ) {
      const int major = regExp.cap( 1 ).toInt();
      const int minor = regExp.cap( 2 ).toInt();
      const int patch = regExp.cap( 3 ).toInt();
      if ( major > 2 || ( major == 2 && ( minor > 3 || ( minor == 3 && patch > 9 ) ) ) ) {
        kDebug( 7116 ) << "Cyrus IMAP >= 2.3.9 detected, enabling shared seen flag support";
        imapCapabilities.append( "x-kmail-sharedseen" );
      }
    }

    kDebug( 7116 ) << "IMAP4::makeLogin - attempting login";

    KIO::AuthInfo authInfo;
    authInfo.username = myUser;
    authInfo.password = myPass;
    authInfo.prompt = i18n( "Username and password for your IMAP account:" );

    kDebug( 7116 ) << "IMAP4::makeLogin - open_PassDlg said user=" << myUser << " pass=xx";

    QString resultInfo;
    if ( myAuth.isEmpty() || myAuth == "*" ) {
      if ( myUser.isEmpty() || myPass.isEmpty() ) {
        if ( openPasswordDialog( authInfo ) ) {
          myUser = authInfo.username;
          myPass = authInfo.password;
        }
      }
      if ( !clientLogin( myUser, myPass, resultInfo ) ) {
        error( ERR_SLAVE_DEFINED, i18n( "Unable to login. Probably the password is wrong.\n"
                                        "The server %1 replied:\n%2",
                                        myHost, resultInfo ) );
      }
    } else {
      if ( !clientAuthenticate( this, authInfo, myHost, myAuth, mySSL, resultInfo ) ) {
        error( ERR_SLAVE_DEFINED, i18n( "Unable to authenticate via %1.\n"
                                        "The server %2 replied:\n%3",
                                        myAuth, myHost, resultInfo ) );
      } else {
        myUser = authInfo.username;
        myPass = authInfo.password;
      }
    }
    if ( hasCapability( "NAMESPACE" ) ) {
      // get all namespaces and save the namespace - delimiter association
      cmd = doCommand( imapCommand::clientNamespace() );
      if ( cmd->result() == "OK" ) {
        kDebug( 7116 ) << "makeLogin - registered namespaces";
      }
      completeQueue.removeAll( cmd );
    }
    // get the default delimiter (empty listing)
    cmd = doCommand( imapCommand::clientList( "", "" ) );
    if ( cmd->result() == "OK" ) {
      QList< imapList >::Iterator it = listResponses.begin();
      if ( it != listResponses.end() ) {
        namespaceToDelimiter[QString()] = ( *it ).hierarchyDelimiter();
        kDebug( 7116 ) << "makeLogin - delimiter for empty ns='" << ( *it ).hierarchyDelimiter() << "'";
        if ( !hasCapability( "NAMESPACE" ) ) {
          // server does not support namespaces
          QString nsentry = QString::number( 0 ) + "==" + ( *it ).hierarchyDelimiter();
          imapNamespaces.append( nsentry );
        }
      }
    }
    completeQueue.removeAll( cmd );
  } else {
    kDebug( 7116 ) << "makeLogin - NO login";
  }

  return getState() == ISTATE_LOGIN;
}

void
IMAP4Protocol::parseWriteLine (const QString & aStr)
{
  //kDebug( 7116 ) << "Writing:" << aStr;
  QByteArray writer = aStr.toUtf8();
  int len = writer.length();

  // append CRLF if necessary
  if ( len == 0 || ( writer[len - 1] != '\n' ) ) {
    len += 2;
    writer += "\r\n";
  }

  // write it
  write( writer.data(), len );
}

QString
IMAP4Protocol::getMimeType (enum IMAP_TYPE aType)
{
  switch ( aType ) {
  case ITYPE_DIR:
    return "inode/directory";
    break;

  case ITYPE_BOX:
    return "message/digest";
    break;

  case ITYPE_DIR_AND_BOX:
    return "message/directory";
    break;

  case ITYPE_MSG:
    return "message/rfc822";
    break;

  // this should be handled by flushOutput
  case ITYPE_ATTACH:
    return "application/octet-stream";
    break;

  case ITYPE_UNKNOWN:
  default:
    return "unknown/unknown";
  }
}

void
IMAP4Protocol::doListEntry (const KUrl & _url, int stretch, imapCache * cache,
  bool withFlags, bool withSubject)
{
  KUrl aURL = _url;
  aURL.setQuery( QString() );
  const QString encodedUrl = aURL.url( KUrl::LeaveTrailingSlash ); // utf-8
  doListEntry( encodedUrl, stretch, cache, withFlags, withSubject );
}

void
IMAP4Protocol::doListEntry (const QString & encodedUrl, int stretch, imapCache * cache,
  bool withFlags, bool withSubject)
{
  if ( cache ) {
    UDSEntry entry;

    entry.clear();

    const QString uid = QString::number( cache->getUid() );
    QString tmp = uid;
    if ( stretch > 0 ) {
      tmp = "0000000000000000" + uid;
      tmp = tmp.right( stretch );
    }
    if ( withSubject ) {
      mailHeader *header = cache->getHeader();
      if ( header ) {
        tmp += ' ' + header->getSubject();
      }
    }
    entry.insert( UDSEntry::UDS_NAME, tmp );

    tmp = encodedUrl; // utf-8
    if ( tmp[tmp.length() - 1] != '/' ) {
      tmp += '/';
    }
    tmp += ";UID=" + uid;
      entry.insert( UDSEntry::UDS_URL, tmp );

      entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFREG );

      entry.insert( UDSEntry::UDS_SIZE, cache->getSize() );

      entry.insert( UDSEntry::UDS_MIME_TYPE, QString::fromLatin1( "message/rfc822" ) );

      entry.insert( UDSEntry::UDS_USER, myUser );

      entry.insert( KIO::UDSEntry::UDS_ACCESS, ( withFlags ) ? cache->getFlags() : S_IRUSR | S_IXUSR | S_IWUSR );

    listEntry( entry, false );
  }
}

void
IMAP4Protocol::doListEntry (const KUrl & _url, const QString & myBox,
                            const imapList & item, bool appendPath)
{
  KUrl aURL = _url;
  aURL.setQuery( QString() );
  UDSEntry entry;
  int hdLen = item.hierarchyDelimiter().length();

  {
    // mailboxName will be appended to the path if appendPath is true
    QString mailboxName = item.name();

    // some beautification
    if ( mailboxName.startsWith( myBox ) && mailboxName.length() > myBox.length() ) {
      mailboxName =
        mailboxName.right( mailboxName.length() - myBox.length() );
    }
    if ( mailboxName[0] == '/' ) {
        mailboxName = mailboxName.right( mailboxName.length() - 1 );
    }
    if ( mailboxName.left( hdLen ) == item.hierarchyDelimiter() ) {
      mailboxName = mailboxName.right( mailboxName.length() - hdLen );
    }
    if ( mailboxName.right( hdLen ) == item.hierarchyDelimiter() ) {
      mailboxName.truncate( mailboxName.length() - hdLen );
    }

    QString tmp;
    if ( !item.hierarchyDelimiter().isEmpty() &&
         mailboxName.contains( item.hierarchyDelimiter() ) ) {
      tmp = mailboxName.section( item.hierarchyDelimiter(), -1 );
    } else {
      tmp = mailboxName;
    }

    // konqueror will die with an assertion failure otherwise
    if ( tmp.isEmpty() ) {
      tmp = "..";
    }

    if ( !tmp.isEmpty() ) {
      entry.insert( UDSEntry::UDS_NAME, tmp );

      if ( !item.noSelect() ) {
        if ( !item.noInferiors() ) {
          tmp = "message/directory";
        } else {
          tmp = "message/digest";
        }
        entry.insert( UDSEntry::UDS_MIME_TYPE, tmp );

        mailboxName += '/';

        // explicitly set this as a directory for KFileDialog
        entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFDIR );
      } else if ( !item.noInferiors() ) {
        entry.insert( UDSEntry::UDS_MIME_TYPE, QString::fromLatin1( "inode/directory" ) );
        mailboxName += '/';

        // explicitly set this as a directory for KFileDialog
        entry.insert( UDSEntry::UDS_FILE_TYPE, S_IFDIR );
      } else {
        entry.insert( UDSEntry::UDS_MIME_TYPE, QString::fromLatin1( "unknown/unknown" ) );
      }

      QString path = aURL.path();
      if ( appendPath ) {
        if ( path[path.length() - 1] == '/' && !path.isEmpty() && path != "/" ) {
          path.truncate( path.length() - 1 );
        }
        if ( !path.isEmpty() && path != "/" &&
             path.right( hdLen ) != item.hierarchyDelimiter() ) {
          path += item.hierarchyDelimiter();
        }
        path += mailboxName;
        if ( path.toUpper() == "/INBOX/" ) {
          // make sure the client can rely on INBOX
          path = path.toUpper();
        }
      }
      aURL.setPath( path );
      tmp = aURL.url( KUrl::LeaveTrailingSlash ); // utf-8
      entry.insert( UDSEntry::UDS_URL, tmp );

      entry.insert( UDSEntry::UDS_USER, myUser );

      entry.insert( UDSEntry::UDS_ACCESS, S_IRUSR | S_IXUSR | S_IWUSR );

      entry.insert( UDSEntry::UDS_EXTRA, item.attributesAsString() );

      listEntry( entry, false );
    }
  }
}

enum IMAP_TYPE
IMAP4Protocol::parseURL (const KUrl & _url, QString & _box,
                         QString & _section, QString & _type, QString & _uid,
                         QString & _validity, QString & _hierarchyDelimiter,
                         QString & _info, bool cache)
{
  enum IMAP_TYPE retVal;
  retVal = ITYPE_UNKNOWN;

  imapParser::parseURL( _url, _box, _section, _type, _uid, _validity, _info );
//  kDebug( 7116 ) << "URL: query - '" << KUrl::fromPercentEncoding(_url.query()) << "'";

  // get the delimiter
  QString myNamespace = namespaceForBox( _box );
  kDebug( 7116 ) << "IMAP4::parseURL - namespace=" << myNamespace;
  if ( namespaceToDelimiter.contains( myNamespace ) ) {
    _hierarchyDelimiter = namespaceToDelimiter[myNamespace];
    kDebug( 7116 ) << "IMAP4::parseURL - delimiter=" << _hierarchyDelimiter;
  }

  if ( !_box.isEmpty() ) {
    kDebug( 7116 ) << "IMAP4::parseURL - box=" << _box;

    if ( makeLogin() ) {
      if ( getCurrentBox() != _box ||
          _type == "LIST" ||
          _type == "LSUB" ||
          _type == "LSUBNOCHECK" ) {
        if ( cache ) {
          // assume a normal box
          retVal = ITYPE_DIR_AND_BOX;
        } else {
          // start a listing for the box to get the type
          CommandPtr cmd;

          cmd = doCommand( imapCommand::clientList( "", _box ) );
          if ( cmd->result() == "OK" ) {
            for ( QList< imapList >::Iterator it = listResponses.begin();
                it != listResponses.end(); ++it ) {
              //kDebug( 7116 ) << "IMAP4::parseURL - checking" << _box << " to" << ( *it ).name();
              if ( _box == ( *it ).name() ) {
                if ( !( *it ).hierarchyDelimiter().isEmpty() ) {
                  _hierarchyDelimiter = ( *it ).hierarchyDelimiter();
                }
                if ( ( *it ).noSelect() ) {
                  retVal = ITYPE_DIR;
                } else if ( ( *it ).noInferiors() ) {
                  retVal = ITYPE_BOX;
                } else {
                  retVal = ITYPE_DIR_AND_BOX;
                }
              }
            }
            // if we got no list response for the box see if it's a prefix
            if ( retVal == ITYPE_UNKNOWN &&
                 namespaceToDelimiter.contains( _box ) ) {
              retVal = ITYPE_DIR;
            }
          } else {
            kDebug( 7116 ) << "IMAP4::parseURL - got error for" << _box;
          }
          completeQueue.removeAll( cmd );
        } // cache
      } else { // current == box
        retVal = ITYPE_BOX;
      }
    } else {
      kDebug( 7116 ) << "IMAP4::parseURL: no login!";
    }

  } else { // empty box
    // the root is just a dir
    kDebug( 7116 ) << "IMAP4::parseURL: box [root]";
    retVal = ITYPE_DIR;
  }

  // see if it is a real sequence or a simple uid
  if ( retVal == ITYPE_BOX || retVal == ITYPE_DIR_AND_BOX ) {
    if ( !_uid.isEmpty() ) {
      if ( !_uid.contains( ':' ) && !_uid.contains( ',' ) && !_uid.contains( '*' ) ) {
        retVal = ITYPE_MSG;
      }
    }
  }
  if ( retVal == ITYPE_MSG ) {
    if ( ( _section.contains( "BODY.PEEK[", Qt::CaseInsensitive ) ||
          _section.contains( "BODY[", Qt::CaseInsensitive ) ) &&
         !_section.contains( ".MIME" ) &&
         !_section.contains( ".HEADER" ) )
      retVal = ITYPE_ATTACH;
  }
  if ( _hierarchyDelimiter.isEmpty() &&
       ( _type == "LIST" || _type == "LSUB" || _type == "LSUBNOCHECK" ) ) {
    // this shouldn't happen but when the delimiter is really empty
    // we try to reconstruct it from the URL
    if ( !_box.isEmpty() ) {
      int start = _url.path().lastIndexOf( _box );
      if ( start != -1 ) {
        _hierarchyDelimiter = _url.path().mid( start - 1, start );
      }
      kDebug( 7116 ) << "IMAP4::parseURL - reconstructed delimiter:" << _hierarchyDelimiter
                     << "from URL" << _url.path();
    }
    if ( _hierarchyDelimiter.isEmpty() ) {
      _hierarchyDelimiter = '/';
    }
  }
  kDebug( 7116 ) << "IMAP4::parseURL - return" << retVal;

  return retVal;
}

int
IMAP4Protocol::outputLine (const QByteArray & _str, int len)
{
  if ( len == -1 ) {
    len = _str.length();
  }

  if ( cacheOutput ) {
    if ( !outputBuffer.isOpen() ) {
      outputBuffer.open( QIODevice::WriteOnly );
    }
    outputBuffer.seek( outputBufferIndex );
    outputBuffer.write( _str.data(), len );
    outputBufferIndex += len;
    return 0;
  }

  QByteArray temp;
  bool relay = relayEnabled;

  relayEnabled = true;
  temp = QByteArray::fromRawData( _str.data(), len );
  parseRelay( temp );
  temp.clear();

  relayEnabled = relay;
  return 0;
}

void IMAP4Protocol::flushOutput(const QString &contentEncoding)
{
  // send out cached data to the application
  if ( outputBufferIndex == 0 ) {
    return;
  }
  outputBuffer.close();
  outputCache.resize( outputBufferIndex );
  if ( decodeContent ) {
    // get the coding from the MIME header
    QByteArray decoded;
    if ( contentEncoding.startsWith( QLatin1String( "quoted-printable" ), Qt::CaseInsensitive ) ) {
      decoded = KCodecs::quotedPrintableDecode( outputCache );
    } else if ( contentEncoding.startsWith( QLatin1String( "base64" ), Qt::CaseInsensitive ) ) {
      decoded = QByteArray::fromBase64( outputCache );
    } else {
      decoded = outputCache;
    }

    QString mimetype = KMimeType::findByContent( decoded )->name();
    kDebug( 7116 ) << "IMAP4::flushOutput - mimeType" << mimetype;
    mimeType( mimetype );
    decodeContent = false;
    data( decoded );
  } else {
    data( outputCache );
  }
  mProcessedSize += outputBufferIndex;
  processedSize( mProcessedSize );
  outputBufferIndex = 0;
  outputCache[0] = '\0';
  outputBuffer.setBuffer( &outputCache );
}

ssize_t IMAP4Protocol::myRead(void *data, ssize_t len)
{
  if ( readBufferLen ) {
    ssize_t copyLen = ( len < readBufferLen ) ? len : readBufferLen;
    memcpy( data, readBuffer, copyLen );
    readBufferLen -= copyLen;
    if ( readBufferLen ) {
      memmove( readBuffer, &readBuffer[copyLen], readBufferLen );
    }
    return copyLen;
  }
  if ( !isConnected() ) {
    return 0;
  }
  waitForResponse( responseTimeout() );
  return read( (char*)data, len );
}

bool
IMAP4Protocol::assureBox (const QString & aBox, bool readonly)
{
  if ( aBox.isEmpty() ) {
    return false;
  }

  CommandPtr cmd;

  if ( aBox != getCurrentBox() || ( !getSelected().readWrite() && !readonly ) ) {
    // open the box with the appropriate mode
    kDebug( 7116 ) << "IMAP4Protocol::assureBox - opening box";
    selectInfo = imapInfo();
    cmd = doCommand( imapCommand::clientSelect( aBox, readonly ) );
    bool ok = cmd->result() == "OK";
    QString cmdInfo = cmd->resultInfo();
    completeQueue.removeAll( cmd );

    if ( !ok ) {
      bool found = false;
      cmd = doCommand( imapCommand::clientList( "", aBox ) );
      if ( cmd->result() == "OK" ) {
        for ( QList< imapList >::Iterator it = listResponses.begin();
             it != listResponses.end(); ++it ) {
          if ( aBox == ( *it ).name() ) {
            found = true;
          }
        }
      }
      completeQueue.removeAll( cmd );
      if ( found ) {
        if ( cmdInfo.contains( "permission", Qt::CaseInsensitive ) ) {
          // not allowed to enter this folder
          error( ERR_ACCESS_DENIED, cmdInfo );
        } else {
          error( ERR_SLAVE_DEFINED, i18n( "Unable to open folder %1. The server replied: %2",
                                          aBox, cmdInfo ) );
        }
      } else {
        error( KIO::ERR_DOES_NOT_EXIST, aBox );
      }
      return false;
    }
  } else {
    // Give the server a chance to deliver updates every ten seconds.
    // Doing this means a server roundtrip and since assureBox is called
    // after every mail, we do it with a timeout.
    kDebug( 7116 ) << "IMAP4Protocol::assureBox - reusing box";
    if ( mTimeOfLastNoop.secsTo( QDateTime::currentDateTime() ) > 10 ) {
      cmd = doCommand( imapCommand::clientNoop() );
      completeQueue.removeAll( cmd );
      mTimeOfLastNoop = QDateTime::currentDateTime();
      kDebug( 7116 ) << "IMAP4Protocol::assureBox - noop timer fired";
    }
  }

  // if it is the mode we want
  if ( !getSelected().readWrite() && !readonly ) {
    error( KIO::ERR_CANNOT_OPEN_FOR_WRITING, aBox );
    return false;
  }

  return true;
}
