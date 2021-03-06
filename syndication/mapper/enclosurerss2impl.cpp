/*
 * This file is part of the syndication library
 *
 * Copyright (C) 2006 Frank Osterfeld <osterfeld@kde.org>
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
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "enclosurerss2impl.h"
#include <constants.h>

#include <QtCore/QRegExp>
#include <QtCore/QString>
#include <QtCore/QStringList>

namespace Syndication {

EnclosureRSS2Impl::EnclosureRSS2Impl(const Syndication::RSS2::Item& item,
                                     const Syndication::RSS2::Enclosure& enc)
    : m_item(item), m_enclosure(enc)
{}
        
bool EnclosureRSS2Impl::isNull() const
{
    return m_enclosure.isNull();
}

QString EnclosureRSS2Impl::url() const
{
    return m_enclosure.url();
}

QString EnclosureRSS2Impl::title() const
{
    // RSS2 enclosures have no title
    return QString();
}

QString EnclosureRSS2Impl::type() const
{
    return m_enclosure.type();
}

uint EnclosureRSS2Impl::length() const
{
    return m_enclosure.length();
}

uint EnclosureRSS2Impl::duration() const
{
    QString durStr = m_item.extractElementTextNS(itunesNamespace(), QLatin1String("duration"));
    
    if (durStr.isEmpty())
        return 0;
    
    QStringList strTokens = durStr.split(QLatin1String(":"));
    QList<int> intTokens;
    
    int count = strTokens.count();
    bool ok;
    
    for (int i = 0; i < count; ++i)
    {
        int intVal = strTokens.at(i).toInt(&ok);
        if (ok)
        {
            intTokens.append(intVal >= 0 ? intVal : 0); // do not accept negative values
        }
        else
            return 0;
    }
    
    if (count == 3)
    {
        return intTokens.at(0) * 3600 + intTokens.at(1) * 60 + intTokens.at(2);
    }
    else if (count == 2)
    {
        return intTokens.at(0) * 60 + intTokens.at(1);
    }
    else if (count == 1)
    {
        return intTokens.at(0);
    }
    
    return 0;
}

} // namespace Syndication
