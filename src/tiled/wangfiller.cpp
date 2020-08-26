/*
 * wangfiller.cpp
 * Copyright 2017, Benjamin Trotter <bdtrotte@ucsc.edu>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "wangfiller.h"

#include "grid.h"
#include "randompicker.h"
#include "staggeredrenderer.h"
#include "tilelayer.h"
#include "wangset.h"

using namespace Tiled;

static const QPoint aroundTilePoints[] = {
    QPoint( 0, -1),
    QPoint( 1, -1),
    QPoint( 1,  0),
    QPoint( 1,  1),
    QPoint( 0,  1),
    QPoint(-1,  1),
    QPoint(-1,  0),
    QPoint(-1, -1)
};

WangFiller::WangFiller(const WangSet &wangSet,
                       const StaggeredRenderer *staggeredRenderer)
    : mWangSet(wangSet)
    , mStaggeredRenderer(staggeredRenderer)
{
}

static void getSurroundingPoints(QPoint point,
                                 const StaggeredRenderer *staggeredRenderer,
                                 QPoint *points)
{
    if (staggeredRenderer) {
        points[0] = staggeredRenderer->topRight(point.x(), point.y());
        points[2] = staggeredRenderer->bottomRight(point.x(), point.y());
        points[4] = staggeredRenderer->bottomLeft(point.x(), point.y());
        points[6] = staggeredRenderer->topLeft(point.x(), point.y());

        if (staggeredRenderer->map()->staggerAxis() == Map::StaggerX) {
            points[1] = point + QPoint(2, 0);
            points[3] = point + QPoint(0, 1);
            points[5] = point + QPoint(-2, 0);
            points[7] = point + QPoint(0, -1);
        } else {
            points[1] = point + QPoint(1, 0);
            points[3] = point + QPoint(0, 2);
            points[5] = point + QPoint(-1, 0);
            points[7] = point + QPoint(0, -2);
        }
    } else {
        for (int i = 0; i < 8; ++i)
            points[i] = point + aroundTilePoints[i];
    }
}

Cell WangFiller::findFittingCell(const TileLayer &back,
                                 const TileLayer &front,
                                 const QRegion &fillRegion,
                                 QPoint point) const
{
    const WangId wangId = wangIdFromSurroundings(back, front, fillRegion, point);
    const auto wangTilesList = mWangSet.findMatchingWangTiles(wangId);

    RandomPicker<WangTile> wangTiles;

    for (const WangTile &wangTile : wangTilesList)
        wangTiles.add(wangTile, mWangSet.wangTileProbability(wangTile));

    if (wangTiles.isEmpty())
        return {};

    // If the Wang set is complete (has all possible combinations), we can pick
    // any of the fitting Wang tiles.
    if (mWangSet.isComplete())
        return wangTiles.pick().makeCell();

    WangTile wangTile;

    // If the set is not complete, we're going to be more careful, trying to
    // pick only Wang tiles for which we can find fitting neighbors on all
    // sides (a rather expensive check, though without it we'll often get
    // situations where we can't find a fitting Wang tile).
    while (!wangTiles.isEmpty()) {
        wangTile = wangTiles.take();

        bool continueFlag = false;

        QPoint adjacentPoints[8];
        getSurroundingPoints(point, mStaggeredRenderer, adjacentPoints);

        // now goes through and checks adjacents, continuing if any can't be filled
        for (int i = 0; i < 8; ++i) {
            QPoint adjacentPoint = adjacentPoints[i];

            // check if the point is empty, otherwise, continue.
            if (!getCell(back, front, fillRegion, adjacentPoint).isEmpty())
                continue;

            WangId adjacentWangId = wangIdFromSurroundings(back,
                                                           front,
                                                           fillRegion,
                                                           adjacentPoint);
            WangId wangId = wangTile.wangId();
            adjacentWangId.updateToAdjacent(wangId, WangId::oppositeIndex(i));

            if (!mWangSet.wildWangIdIsUsed(adjacentWangId)) {
                continueFlag = true;
                break;
            }
        }

        if (!continueFlag)
            break;
    }

    return wangTile.makeCell();
}

void WangFiller::fillRegion(TileLayer &target,
                            const TileLayer &back,
                            const QRegion &region) const
{
    return fillRegion(target, back, Grid<WangId>(), region);
}

static WangTile findBestMatch(const WangSet &wangSet, WangId wangId)
{
    const unsigned mask = wangId.mask();
    const unsigned maskedWangId = wangId & mask;

    RandomPicker<WangTile> matches;
    int lowestPenalty = INT_MAX;

    // TODO: this is a slow linear search, perhaps we could use a better find algorithm...
    for (const WangTile &wangTile : wangSet.wangTilesByWangId()) {
        if ((wangTile.wangId() & mask) != maskedWangId)
            continue;

        int penalty = 0;
        for (int i = 0; i < WangId::NumIndexes; ++i)
            if (wangTile.wangId().indexColor(i) != wangId.indexColor(i))
                ++penalty;

        // add tile to the candidate list
        if (penalty <= lowestPenalty) {
            if (penalty < lowestPenalty) {
                matches.clear();
                lowestPenalty = penalty;
            }

            matches.add(wangTile, wangSet.wangTileProbability(wangTile));
        }
    }

    // choose a candidate at random, with consideration for probability
    if (!matches.isEmpty())
        return matches.pick();

    return WangTile();
}

void WangFiller::fillRegion(TileLayer &target,
                            const TileLayer &back,
                            Grid<WangId> wangIds,
                            const QRegion &region) const
{
    // Set the Wang IDs at the border of the region to make sure the tiles in
    // the filled region connect with those outside of it.
#if QT_VERSION < 0x050800
    const auto rects = fillRegion.rects();
    for (const QRect &rect : rects) {
#else
    for (const QRect &rect : region) {
#endif
        for (int x = rect.left(); x <= rect.right(); ++x) {
            // TODO: Handle staggered maps
            const QPoint top(x, rect.top() - 1);
            const QPoint bottom(x, rect.bottom() + 1);

            if (!region.contains(top)) {
                WangId topWangId = mWangSet.wangIdOfCell(back.cellAt(top));
                WangId wangId = wangIds.get(x, rect.top());
                wangId.mergeFromAdjacent(topWangId, WangId::Top);
                wangIds.set(x, rect.top(), wangId);
            }

            if (!region.contains(bottom)) {
                WangId bottomWangId = mWangSet.wangIdOfCell(back.cellAt(bottom));
                WangId wangId = wangIds.get(x, rect.bottom());
                wangId.mergeFromAdjacent(bottomWangId, WangId::Bottom);
                wangIds.set(x, rect.bottom(), wangId);
            }
        }

        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            // TODO: Handle staggered maps
            const QPoint left(rect.left() - 1, y);
            const QPoint right(rect.right() + 1, y);

            if (!region.contains(left)) {
                WangId leftWangId = mWangSet.wangIdOfCell(back.cellAt(left));
                WangId wangId = wangIds.get(rect.left(), y);
                wangId.mergeFromAdjacent(leftWangId, WangId::Left);
                wangIds.set(rect.left(), y, wangId);
            }

            if (!region.contains(right)) {
                WangId rightWangId = mWangSet.wangIdOfCell(back.cellAt(right));
                WangId wangId = wangIds.get(rect.right(), y);
                wangId.mergeFromAdjacent(rightWangId, WangId::Right);
                wangIds.set(rect.right(), y, wangId);
            }
        }
    }

#if QT_VERSION < 0x050800
    for (const QRect &rect : rects) {
#else
    for (const QRect &rect : region) {
#endif
        for (int y = rect.top(); y <= rect.bottom(); ++y) {
            for (int x = rect.left(); x <= rect.right(); ++x) {
                const WangTile wangTile = findBestMatch(mWangSet, wangIds.get(x, y));
                if (!wangTile.tile()) {
                    // TODO: error feedback
                    continue;
                }

                target.setCell(x - target.x(),
                               y - target.y(),
                               wangTile.makeCell());

//                qDebug() << x << y << wangIds.get(x, y) << QString::number(wangIds.get(x, y).mask(), 16) << wangTile.wangId();

                // Adjust the desired WangIds for the surrounding tiles based on the placed one
                QPoint adjacentPoints[8];
                getSurroundingPoints(QPoint(x, y), mStaggeredRenderer, adjacentPoints);

                for (int i = 0; i < 8; ++i) {
                    const QPoint p = adjacentPoints[i];
                    if (!target.cellAt(p - target.position()).isEmpty())
                        continue;

                    WangId adjacentWangId = wangIds.get(p);
                    adjacentWangId.updateToAdjacent(wangTile.wangId(), WangId::oppositeIndex(i));
                    wangIds.set(p, adjacentWangId);
                }
            }
        }
    }
}

const Cell &WangFiller::getCell(const TileLayer &back,
                                const TileLayer &front,
                                const QRegion &fillRegion,
                                QPoint point) const
{
    if (fillRegion.contains(point))
        return front.cellAt(point.x() - front.x(), point.y() - front.y());
    else
        return back.cellAt(point);
}

WangId WangFiller::wangIdFromSurroundings(const TileLayer &back,
                                          const TileLayer &front,
                                          const QRegion &fillRegion,
                                          QPoint point) const
{
    Cell surroundingCells[8];
    QPoint adjacentPoints[8];
    getSurroundingPoints(point, mStaggeredRenderer, adjacentPoints);

    for (int i = 0; i < 8; ++i)
        surroundingCells[i] = getCell(back, front, fillRegion, adjacentPoints[i]);

    return mWangSet.wangIdFromSurrounding(surroundingCells);
}

WangId WangFiller::wangIdFromSurroundings(const TileLayer &back,
                                          const QRegion &fillRegion,
                                          QPoint point) const
{
    Cell surroundingCells[8];
    QPoint adjacentPoints[8];
    getSurroundingPoints(point, mStaggeredRenderer, adjacentPoints);

    for (int i = 0; i < 8; ++i) {
        if (!fillRegion.contains(adjacentPoints[i]))
            surroundingCells[i] = back.cellAt(adjacentPoints[i]);
    }

    return mWangSet.wangIdFromSurrounding(surroundingCells);
}
