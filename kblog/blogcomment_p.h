/*
  This file is part of the kblog library.

  Copyright (c) 2006-2007 Christian Weilbach <christian_weilbach@web.de>
  Copyright (c) 2007 Mike McQuaid <mike@mikemcquaid.com>

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

#ifndef BLOGCOMMENT_P_H
#define BLOGCOMMENT_P_H

#include "blogcomment.h"

#include <KDateTime>
#include <KUrl>

namespace KBlog{

class BlogCommentPrivate
{
  public:
    BlogComment *q_ptr;
    QString mTitle;
    QString mContent;
    QString mEmail;
    QString mName;
    QString mCommentId;
    KUrl mUrl;
    QString mError;
    BlogComment::Status mStatus;
    KDateTime mModificationDateTime;
    KDateTime mCreationDateTime;
};

} // namespace KBlog
#endif
