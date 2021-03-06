/*##############################################################################

    Copyright (C) 2011 HPCC Systems.

    All rights reserved. This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
############################################################################## */

#include "thexception.hpp"
#include "jhtree.hpp"
#include "dasess.hpp"
#include "dadfs.hpp"

#include "thdiskbase.ipp"
#include "thindexread.ipp"

class CIndexReadBase : public CMasterActivity
{
protected:
    BoolArray performPartLookup;
    Linked<IDistributedFile> index;
    Owned<IFileDescriptor> fileDesc;
    rowcount_t limit;
    IHThorIndexReadBaseArg *helper;
    Owned<CSlavePartMapping> mapping;
    bool nofilter;
    ProgressInfoArray progressInfoArr;
    StringArray progressLabels;
    Owned<ProgressInfo> inputProgress;

    rowcount_t aggregateToLimit()
    {
        rowcount_t total = 0;
        ICommunicator &comm = container.queryJob().queryJobComm();
        unsigned slaves = container.queryJob().querySlaves();
        unsigned s;
        for (s=0; s<slaves; s++)
        {
            CMessageBuffer msg;
            rank_t sender;
            if (!receiveMsg(msg, RANK_ALL, mpTag, &sender))
                return 0;
            if (abortSoon)
                return 0;
            rowcount_t count;
            msg.read(count);
            total += count;
            if (total > limit)
                break;
        }
        return total;
    }
    void prepareKey()
    {
        IDistributedFile *f = index.get();
        IDistributedSuperFile *super = f->querySuperFile();

        unsigned nparts = f->numParts(); // includes tlks if any, but unused in array
        performPartLookup.ensure(nparts);

        bool checkTLKConsistency = NULL != super && 0 != (TIRsorted & helper->getFlags());
        if (nofilter)
        {
            while (nparts--) performPartLookup.append(true);
            if (!checkTLKConsistency) return;
        }
        else
        {
            while (nparts--) performPartLookup.append(false); // parts to perform lookup set later
        }

        Owned<IDistributedFileIterator> iter;
        if (super) {
            iter.setown(super->getSubFileIterator(true));
            verifyex(iter->first());
            f = &iter->query();
        }
        unsigned width = f->numParts()-1;
        assertex(width);
        unsigned tlkCrc;
        bool first = true;
        unsigned superSubIndex=0;
        bool fileCrc = false, rowCrc = false;
        loop {
            Owned<IDistributedFilePart> part = f->getPart(width);
            if (checkTLKConsistency)
            {
                unsigned _tlkCrc;
                if (part->getCrc(_tlkCrc))
                    fileCrc = true;
                else if (part->queryProperties().hasProp("@crc")) // NB: key "@crc" is not a crc on the file, but data within.
                {
                    _tlkCrc = part->queryProperties().getPropInt("@crc");
                    rowCrc = true;
                }
                else if (part->queryProperties().hasProp("@tlkCrc")) // backward compat.
                {
                    _tlkCrc = part->queryProperties().getPropInt("@tlkCrc");
                    rowCrc = true;
                }
                else
                {
                    if (rowCrc || fileCrc)
                    {
                        checkTLKConsistency = false;
                        Owned<IException> e = MakeActivityWarning(&container, 0, "Cannot validate that tlks in superfile %s match, some crc attributes are missing", super->queryLogicalName());
                        container.queryJob().fireException(e);
                    }       
                }
                if (rowCrc && fileCrc)
                {
                    checkTLKConsistency = false;
                    Owned<IException> e = MakeActivityWarning(&container, 0, "Cannot validate that tlks in superfile %s match, due to mixed crc types.", super->queryLogicalName());
                    container.queryJob().fireException(e);
                }
                if (checkTLKConsistency)
                {
                    if (first)
                    {
                        tlkCrc = _tlkCrc;
                        first = false;
                    }
                    else if (tlkCrc != _tlkCrc)
                        throw MakeActivityException(this, 0, "Sorted output on super files comprising of non coparitioned sub keys is not supported (TLK's do not match)");
                }
            }
            if (!nofilter)
            {
                Owned<IKeyIndex> keyIndex;
                unsigned copy;
                for (copy=0; copy<part->numCopies(); copy++)
                {
                    RemoteFilename rfn;
                    OwnedIFile ifile = createIFile(part->getFilename(rfn,copy));
                    if (ifile->exists())
                    {
                        StringBuffer remotePath;
                        rfn.getRemotePath(remotePath);
                        unsigned crc;
                        part->getCrc(crc);
                        keyIndex.setown(createKeyIndex(remotePath.str(), crc, false, false));
                        break;
                    }
                }
                if (!keyIndex)
                    throw MakeThorException(TE_FileNotFound, "Top level key part does not exist, for key: %s", helper->getFileName());
            
                unsigned maxSize = helper->queryDiskRecordSize()->querySerializedMeta()->getRecordSize(NULL); // used only if fixed
                Owned <IKeyManager> tlk = createKeyManager(keyIndex, maxSize, NULL);
                helper->createSegmentMonitors(tlk);
                tlk->finishSegmentMonitors();
                tlk->reset();
                while (tlk->lookup(false))
                {
                    if (tlk->queryFpos())
                        performPartLookup.replace(true, (aindex_t)(super?super->numSubFiles(true)*(tlk->queryFpos()-1)+superSubIndex:tlk->queryFpos()-1));
                }
            }
            if (!super||!iter->next())
                break;
            superSubIndex++;
            f = &iter->query();
            if (width != f->numParts()-1)
                throw MakeActivityException(this, 0, "Super key %s, with mixture of sub key width are not supported.", f->queryLogicalName());
        }
    }

public:
    CIndexReadBase(CMasterGraphElement *info) : CMasterActivity(info)
    {
        limit = RCMAX;
        if (!container.queryLocalOrGrouped())
            mpTag = container.queryJob().allocateMPTag();
        progressLabels.append("seeks");
        progressLabels.append("scans");
        ForEachItemIn(l, progressLabels)
            progressInfoArr.append(*new ProgressInfo);
        inputProgress.setown(new ProgressInfo);
    }
    void init()
    {
        helper = (IHThorIndexReadArg *)queryHelper();
        nofilter = false;

        index.setown(queryThorFileManager().lookup(container.queryJob(), helper->getFileName(), false, 0 != (TIRoptional & helper->getFlags()), true));
        if (index)
        {
            nofilter = 0 != (TIRnofilter & helper->getFlags());
            if (index->queryProperties().getPropBool("@local"))
                nofilter = true;
            else
            {
                IDistributedSuperFile *super = index->querySuperFile();
                IDistributedFile *sub = super ? &super->querySubFile(0,true) : index.get();
                if (sub && 1 == sub->numParts())
                    nofilter = true;
            }   
            checkFormatCrc(this, index, helper->getFormatCrc(), true);
            if ((container.queryLocalOrGrouped() || helper->canMatchAny()) && index->numParts())
            {
                fileDesc.setown(getConfiguredFileDescriptor(*index));
                if (container.queryLocalOrGrouped())
                    nofilter = true;
                prepareKey();
                queryThorFileManager().noteFileRead(container.queryJob(), index);
                mapping.setown(getFileSlaveMaps(index->queryLogicalName(), *fileDesc, container.queryJob().queryUserDescriptor(), container.queryJob().querySlaveGroup(), container.queryLocalOrGrouped(), true, NULL, index->querySuperFile()));
            }
        }
    }
    void serializeSlaveData(MemoryBuffer &dst, unsigned slave)
    {
        dst.append(helper->getFileName());
        if (!container.queryLocalOrGrouped())
            dst.append(mpTag);
        IArrayOf<IPartDescriptor> parts;
        if (fileDesc.get())
        {
            mapping->getParts(slave, parts);
            if (!nofilter)
            {
                ForEachItemInRev(p, parts)
                {
                    IPartDescriptor &part = parts.item(p);
                    if (!performPartLookup.item(part.queryPartIndex()))
                        parts.zap(part);
                }
            }
        }
        dst.append((unsigned)parts.ordinality());
        UnsignedArray partNumbers;
        ForEachItemIn(p2, parts)
            partNumbers.append(parts.item(p2).queryPartIndex());
        if (partNumbers.ordinality())
            fileDesc->serializeParts(dst, partNumbers);
    }
    void deserializeStats(unsigned node, MemoryBuffer &mb)
    {
        CMasterActivity::deserializeStats(node, mb);
        rowcount_t progress;
        mb.read(progress);
        inputProgress->set(node, progress);
        ForEachItemIn(p, progressLabels)
        {
            unsigned __int64 st;
            mb.read(st);
            progressInfoArr.item(p).set(node, st);
        }
    }
    void getXGMML(unsigned idx, IPropertyTree *edge)
    {
        CMasterActivity::getXGMML(idx, edge);
        StringBuffer label;
        label.append("@inputProgress");
        edge->setPropInt64(label.str(), inputProgress->queryTotal());
        ForEachItemIn(p, progressInfoArr)
        {
            ProgressInfo &progress = progressInfoArr.item(p);
            progress.processInfo();
            StringBuffer attr("@");
            attr.append(progressLabels.item(p));
            edge->setPropInt64(attr.str(), progress.queryTotal());
        }
    }
    virtual void abort()
    {
        CMasterActivity::abort();
        cancelReceiveMsg(RANK_ALL, mpTag);
    }
    void kill()
    {
        CMasterActivity::kill();
        index.clear();
    }
};

class CIndexReadActivityMaster : public CIndexReadBase
{
    IHThorIndexReadArg *helper;

    void processKeyedLimit()
    {
        rowcount_t total = aggregateToLimit();
        if (total > limit)
        {
            if (0 == (TIRkeyedlimitskips & helper->getFlags()))
            {
                if (0 == (TIRkeyedlimitcreates & helper->getFlags()))
                    helper->onKeyedLimitExceeded(); // should throw exception
            }
        }
        CMessageBuffer msg;
        msg.append(total);
        ICommunicator &comm = container.queryJob().queryJobComm();
        unsigned slaves = container.queryJob().querySlaves();
        unsigned s=0;
        for (; s<slaves; s++)
        {
            verifyex(comm.send(msg, s+1, mpTag));
        }
    }
public:
    CIndexReadActivityMaster(CMasterGraphElement *info) : CIndexReadBase(info)
    {
        helper = (IHThorIndexReadArg *)queryHelper();
    }
    void init()
    {
        CIndexReadBase::init();
        if (!container.queryLocalOrGrouped())
        {
            if (helper->canMatchAny())
                limit = (rowcount_t)helper->getKeyedLimit();
        }
    }
    void process()
    {
        if (limit != RCMAX)
            processKeyedLimit();
    }
};


CActivityBase *createIndexReadActivityMaster(CMasterGraphElement *info)
{
    return new CIndexReadActivityMaster(info);
}


class CIndexCountActivityMaster : public CIndexReadBase
{
    IHThorIndexCountArg *helper;
    bool totalCountKnown;
    rowcount_t totalCount;

public:
    CIndexCountActivityMaster(CMasterGraphElement *info) : CIndexReadBase(info)
    {
        helper = (IHThorIndexCountArg *)queryHelper();
        totalCount = 0;
        totalCountKnown = false;
        if (!container.queryLocalOrGrouped())
        {
            if (helper->canMatchAny())
                limit = (rowcount_t)helper->getChooseNLimit();
            else
                totalCountKnown = true; // totalCount = 0
        }
    }
    virtual void process()
    {
        if (container.queryLocalOrGrouped())
            return;
        if (totalCountKnown) return;
        rowcount_t total = aggregateToLimit();
        if (limit != RCMAX && total > limit)
            total = limit;
        CMessageBuffer msg;
        msg.append(total);
        ICommunicator &comm = container.queryJob().queryJobComm();
        verifyex(comm.send(msg, 1, mpTag)); // send to 1st slave only
    }
};

CActivityBase *createIndexCountActivityMaster(CMasterGraphElement *info)
{
    return new CIndexCountActivityMaster(info);
}

class CIndexNormalizeActivityMaster : public CIndexReadBase
{
    IHThorIndexNormalizeArg *helper;

    void processKeyedLimit()
    {
        rowcount_t total = aggregateToLimit();
        if (total > limit)
        {
            if (0 == (TIRkeyedlimitskips & helper->getFlags()))
            {
                if (0 == (TIRkeyedlimitcreates & helper->getFlags()))
                    helper->onKeyedLimitExceeded(); // should throw exception
            }
        }
        CMessageBuffer msg;
        msg.append(total);
        ICommunicator &comm = container.queryJob().queryJobComm();
        unsigned slaves = container.queryJob().querySlaves();
        unsigned s=0;
        for (; s<slaves; s++)
        {
            verifyex(comm.send(msg, s+1, mpTag));
        }
    }
public:
    CIndexNormalizeActivityMaster(CMasterGraphElement *info) : CIndexReadBase(info)
    {
        helper = (IHThorIndexNormalizeArg *)queryHelper();
    }
    virtual void init()
    {
        CIndexReadBase::init();
        if (!container.queryLocalOrGrouped())
        {
            if (helper->canMatchAny())
                limit = (rowcount_t)helper->getKeyedLimit();
        }
    }
    virtual void process()
    {
        if (limit != RCMAX)
            processKeyedLimit();
    }
};

CActivityBase *createIndexNormalizeActivityMaster(CMasterGraphElement *info)
{
    return new CIndexNormalizeActivityMaster(info);
}


class CIndexAggregateActivityMaster : public CIndexReadBase
{
    IHThorIndexAggregateArg *helper;
public:
    CIndexAggregateActivityMaster(CMasterGraphElement *info) : CIndexReadBase(info)
    {
        helper = (IHThorIndexAggregateArg *)queryHelper();
    }
    virtual void process()
    {
        if (container.queryLocalOrGrouped())
            return;
        Owned<IRowInterfaces> rowIf = createRowInterfaces(helper->queryOutputMeta(), queryActivityId(), queryCodeContext());                
        OwnedConstThorRow result = getAggregate(*this, container.queryJob().querySlaves(), *rowIf, *helper, mpTag);
        if (!result)
            return;
        CMessageBuffer msg;
        CMemoryRowSerializer mbs(msg);
        rowIf->queryRowSerializer()->serialize(mbs,(const byte *)result.get());
        if (!container.queryJob().queryJobComm().send(msg, 1, mpTag, 5000))
            throw MakeThorException(0, "Failed to give result to slave");
    }
};


CActivityBase *createIndexAggregateActivityMaster(CMasterGraphElement *info)
{
    return new CIndexAggregateActivityMaster(info);
}

class CIndexGroupAggregateActivityMaster : public CIndexReadBase
{
public:
    CIndexGroupAggregateActivityMaster(CMasterGraphElement *info) : CIndexReadBase(info)
    {
    }
};

CActivityBase *createIndexGroupAggregateActivityMaster(CMasterGraphElement *info)
{
    return new CIndexGroupAggregateActivityMaster(info);
}
