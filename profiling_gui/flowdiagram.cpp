// Copyright (c) 2013 Maciej Gajewski

#include "profiling_gui/flowdiagram.hpp"
#include "profiling_gui/flowdiagram_items.hpp"

#include <QDebug>

#include <random>

namespace profiling_gui {

static const double THREAD_Y_SPACING = 100.0;
static const double CORO_H = 5; // half-heights
static const double BLOCK_H = 2;




FlowDiagram::FlowDiagram(QObject *parent) :
    QObject(parent)
{
}

void FlowDiagram::loadFile(const QString& path, QGraphicsScene* scene, CoroutinesModel& coroutinesModel)
{
    _scene = scene;
    profiling_reader::reader reader(path.toStdString());

    _ticksPerNs = reader.ticks_per_ns();

    // collect data
    reader.for_each_by_time([this](const profiling_reader::record_type& record)
    {
        this->onRecord(record);
    });

    // build threads
    for(auto it = _threads.begin(); it != _threads.end(); it++)
    {
        ThreadData& thread = it.value();
        auto* item = new QGraphicsLineItem(ticksToTime(thread.minTicks), thread.y, ticksToTime(thread.maxTicks), thread.y);
        QPen p(Qt::black);
        p.setCosmetic(true);
        item->setPen(p);
        scene->addItem(item);

        // if there is unfinished block - finish it artificially at the end of thread
        if (thread.lastBlock != INVALID_TICK_VALUE)
        {
            profiling_reader::record_type fakeRecord;
            fakeRecord.object_id = it.key();
            fakeRecord.thread_id = it.key();
            fakeRecord.time = thread.maxTicks;
            fakeRecord.event = "unblock";
            onProcessorRecord(fakeRecord, thread);
        }
    }

    // build coros
    for(auto it = _coroutines.begin(); it != _coroutines.end(); it++)
    {
        const CoroutineData& coro = *it;
        auto* group = new CoroutineGroup(it.key());

        connect(&coroutinesModel, SIGNAL(coroSelected(quintptr)), group, SLOT(onCoroutineSelected(quintptr)));
        connect(group, SIGNAL(coroSelected(quintptr)), &coroutinesModel, SLOT(onCoroutineSelected(quintptr)));

        // if there is open coroutine, finish it at the thread's end
        if (coro.enters.size() == 1)
        {
            auto enterIt = coro.enters.begin();
            const ThreadData& thread = _threads[enterIt.key()];
            // create a fake event
            profiling_reader::record_type fakeRecord;
            fakeRecord.object_id = it.key();
            fakeRecord.thread_id = enterIt.key();
            fakeRecord.time = thread.maxTicks;
            fakeRecord.event = "exit";
            onCoroutineRecord(fakeRecord, thread);
        }
        else if (coro.enters.size() > 1)
        {
            qWarning() << "Coroutine withj more than one unfinished enter. id=" << it.key();
        }

        // group all items and add to scene
        for(QGraphicsItem* item : coro.items)
        {
            item->setParentItem(group);
        }
        scene->addItem(group);

        // add to model
        CoroutinesModel::Record r {
            it.key(),
            coro.name,
            coro.color,
            ticksToTime(coro.totalTime) // time executed, ns
        };
        coroutinesModel.Append(r);
    }

    // fix scene rectangle height, s there is half-spacing margin above and below the firsrt and the last thread
    QRectF sceneRect = _scene->sceneRect();
    sceneRect.setTop(-THREAD_Y_SPACING/2);
    sceneRect.setHeight( THREAD_Y_SPACING * _threads.size());
    _scene->setSceneRect(sceneRect);

    // now that we know the scene size, add line for each thread
    for(const ThreadData& thread : _threads)
    {
        auto* item = new QGraphicsLineItem(sceneRect.left(), thread.y, sceneRect.right(), thread.y);
        item->setPen(QPen(Qt::lightGray));
        item->setZValue(-10);
        _scene->addItem(item);
    }
}

double FlowDiagram::ticksToTime(qint64 ticks) const
{
    return ticks / _ticksPerNs;
}

static QColor randomColor()
{
    static std::minstd_rand0 generator;

    int h = std::uniform_int_distribution<int>(0, 255)(generator);
    int s = 172 + std::uniform_int_distribution<int>(0, 63)(generator);
    int v = 172 + std::uniform_int_distribution<int>(-32, +32)(generator);

    return QColor::fromHsv(h, s, v);
}

void FlowDiagram::onRecord(const profiling_reader::record_type& record)
{
    if (!_threads.contains(record.thread_id))
    {
        ThreadData newThread;
        newThread.minTicks = record.time;
        newThread.y = _threads.size() * THREAD_Y_SPACING;

        _threads.insert(record.thread_id, newThread);
    }

    ThreadData& thread = _threads[record.thread_id];
    thread.maxTicks = record.time;

    if (record.object_type == "processor")
    {
        onProcessorRecord(record, thread);
    }
    else if (record.object_type == "coroutine")
    {
        onCoroutineRecord(record, thread);
    }
}

void FlowDiagram::onProcessorRecord(const profiling_reader::record_type& record, ThreadData& thread)
{
    if (record.event == "block")
    {
        thread.lastBlock = record.time;
    }
    else if (record.event == "unblock")
    {
        if (thread.lastBlock == INVALID_TICK_VALUE)
        {
            qWarning() << "Process: unblock withoiut block! id=" << record.object_id << "time=" << record.time;
        }
        else
        {
            double blockX = ticksToTime(thread.lastBlock);
            double unblockX = ticksToTime(record.time);
            double y = thread.y;

            thread.lastBlock = INVALID_TICK_VALUE;

            auto* item = new QGraphicsRectItem(blockX, y-BLOCK_H, unblockX-blockX, 2*BLOCK_H);
            item->setBrush(Qt::black);
            item->setToolTip("blocked");
            item->setZValue(2.0);
            _scene->addItem(item);
        }
    }
}

void FlowDiagram::onCoroutineRecord(const profiling_reader::record_type& record, const ThreadData& thread)
{
    CoroutineData& coroutine = _coroutines[record.object_id];

    if (!coroutine.color.isValid())
        coroutine.color = randomColor();

    if (record.event == "created")
        coroutine.name = QString::fromStdString(record.data);

    if (record.event == "enter")
    {
        coroutine.enters[record.thread_id] = record.time;
    }

    if (record.event == "exit")
    {
        if(!coroutine.enters.contains(record.thread_id))
        {
            qWarning() << "Corotuine: exit without enter! id=" << record.object_id << ", time= " << record.time << ",thread=" << record.thread_id;
        }
        else
        {
            quint64 enterTicks = coroutine.enters[record.thread_id];
            coroutine.enters.remove(record.thread_id);

            double enterX =  ticksToTime(enterTicks);
            double exitX = ticksToTime(record.time);
            double y = thread.y;

            // block
            auto* item = new SelectableRectangle(enterX, y-CORO_H, exitX-enterX, CORO_H*2);
            item->setToolTip(coroutine.name);
            item->setBrush(coroutine.color);

            coroutine.items.append(item);

            // connection with previous one
            if (!coroutine.lastExit.isNull())
            {
                auto* item = new SelectableLine(coroutine.lastExit.x(), coroutine.lastExit.y(), enterX, y);
                QColor c = coroutine.color;
                QPen pen(c);
                item->setPen(pen);
                coroutine.items.append(item);
            }

            coroutine.lastExit = QPointF(exitX, y);
            coroutine.totalTime += record.time - enterTicks;
        }
    }
}



}