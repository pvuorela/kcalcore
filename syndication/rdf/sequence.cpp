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

#include "sequence.h"
#include "node.h"
#include "nodevisitor.h"

#include <QtCore/QList>
#include <QtCore/QString>

namespace Syndication {
namespace RDF {

class Sequence::SequencePrivate
{
    public:
    
    QList<NodePtr> items;
};

Sequence::Sequence() : Resource(), d()
{
}

Sequence::Sequence(const QString& uri)
    : Resource(uri), d(new SequencePrivate)
{
}

Sequence::Sequence(const Sequence& other) : Resource(other)
{
    *this = other;
}

Sequence::~Sequence()
{
}
void Sequence::accept(NodeVisitor* visitor, NodePtr ptr)
{
    SequencePtr sptr = boost::static_pointer_cast<Sequence>(ptr);
    if (!visitor->visitSequence(sptr))
        Resource::accept(visitor, ptr);
}
                
Sequence* Sequence::clone() const
{
    return new Sequence(*this);
}

Sequence& Sequence::operator=(const Sequence& other)
{
    Resource::operator=(other);
    d = other.d;
    return *this;
}

void Sequence::append(NodePtr node)
{
    if (d)
        d->items.append(node);
}

QList<NodePtr> Sequence::items() const
{
    return d ? d->items : QList<NodePtr>();
}

bool Sequence::isSequence() const
{
    return true;
}

} // namespace RDF
} // namespace Syndication
