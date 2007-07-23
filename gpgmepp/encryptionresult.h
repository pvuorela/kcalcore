/*
  encryptionresult.h - wraps a gpgme sign result
  Copyright (C) 2004 Klarälvdalens Datakonsult AB

  This file is part of GPGME++.

  GPGME++ is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; either
  version 2 of the License, or (at your option) any later version.

  GPGME++ is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Library General Public License for more details.

  You should have received a copy of the GNU Library General Public License
  along with GPGME++; see the file COPYING.LIB.  If not, write to the
  Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301, USA.
*/

#ifndef __GPGMEPP_ENCRYPTIONRESULT_H__
#define __GPGMEPP_ENCRYPTIONRESULT_H__

#include <gpgmepp/gpgmefw.h>
#include <gpgmepp/result.h>
#include <gpgmepp/gpgmepp_export.h>

#include <vector>

namespace GpgME {

  class Error;
  class InvalidRecipient;

  class GPGMEPP_EXPORT EncryptionResult : public Result {
  public:
    explicit EncryptionResult( gpgme_ctx_t ctx=0, int error=0 );
    explicit EncryptionResult( const Error & err );
    EncryptionResult( const EncryptionResult & other );
    ~EncryptionResult();

    const EncryptionResult & operator=( const EncryptionResult & other );

    bool isNull() const;

    unsigned int numInvalidRecipients() const;

    InvalidRecipient invalidEncryptionKey( unsigned int index ) const;
    std::vector<InvalidRecipient> invalidEncryptionKeys() const;

    class Private;
  private:
    Private * d;
  };

  class GPGMEPP_EXPORT InvalidRecipient {
    friend class EncryptionResult;
    InvalidRecipient( EncryptionResult::Private * parent, unsigned int index );
  public:
    InvalidRecipient();
    InvalidRecipient( const InvalidRecipient & other );
    ~InvalidRecipient();

    const InvalidRecipient & operator=( const InvalidRecipient & other );

    bool isNull() const;

    const char * fingerprint() const;
    Error reason() const;

  private:
    EncryptionResult::Private * d;
    unsigned int idx;
  };

}

#endif // __GPGMEPP_ENCRYPTIONRESULT_H__
