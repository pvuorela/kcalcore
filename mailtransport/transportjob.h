  /*
    Copyright (c) 2007 Volker Krause <vkrause@kde.org>

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This library is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.
*/

#ifndef MAILTRANSPORT_TRANSPORTJOB_H
#define MAILTRANSPORT_TRANSPORTJOB_H

#include <mailtransport/mailtransport_export.h>

#include <QtCore/QStringList>

#include <KDE/KCompositeJob>

class QBuffer;

namespace MailTransport {

class Transport;

/**
  Abstract base class for all mail transport jobs.
  This is a job that is supposed to send exactly one mail.

  @deprecated Use MessageQueueJob for sending e-mail.
*/
class MAILTRANSPORT_EXPORT_DEPRECATED TransportJob : public KCompositeJob
{
  friend class TransportManager;

  public:
    /**
      Deletes this transport job.
    */
    virtual ~TransportJob();

    /**
      Sets the sender of the mail.
      @p sender must be the plain email address, not including display name.
    */
    void setSender( const QString &sender );

    /**
      Sets the "To" receiver(s) of the mail.
      @p to must be the plain email address(es), not including display name.
    */
    void setTo( const QStringList &to );

    /**
      Sets the "Cc" receiver(s) of the mail.
      @p cc must be the plain email address(es), not including display name.
    */
    void setCc( const QStringList &cc );

    /**
      Sets the "Bcc" receiver(s) of the mail.
      @p bcc must be the plain email address(es), not including display name.
    */
    void setBcc( const QStringList &bcc );

    /**
      Sets the content of the mail.
    */
    void setData( const QByteArray &data );

    /**
      Starts this job. It is recommended to not call this method directly but use
      TransportManager::schedule() to execute the job instead.

      @see TransportManager::schedule()
    */
    virtual void start();

    /**
      Returns the Transport object containing the mail transport settings.
    */
    Transport *transport() const;

  protected:
    /**
      Creates a new mail transport job.
      @param transport The transport configuration. This must be a deep copy of
      a Transport object, the job takes the ownership of this object.
      @param parent The parent object.
      @see TransportManager::createTransportJob()
    */
    explicit TransportJob( Transport *transport, QObject *parent = 0 );

    /**
      Returns the sender of the mail.
    */
    QString sender() const;

    /**
      Returns the "To" receiver(s) of the mail.
    */
    QStringList to() const;

    /**
      Returns the "Cc" receiver(s) of the mail.
    */
    QStringList cc() const;

    /**
      Returns the "Bcc" receiver(s) of the mail.
    */
    QStringList bcc() const;

    /**
      Returns the data of the mail.
    */
    QByteArray data() const;

    /**
      Returns a QBuffer opened on the message data. This is useful for
      processing the data in smaller chunks.
    */
    QBuffer *buffer();

    /**
      Do the actual work, implement in your subclass.
    */
    virtual void doStart() = 0;

  private:
    //@cond PRIVATE
    class Private;
    Private *const d;
    //@endcond
};

} // namespace MailTransport

#endif // MAILTRANSPORT_TRANSPORTJOB_H
