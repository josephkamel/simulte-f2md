    //
//                           SimuLTE
//
// This file is part of a software released under the license included in file
// "license.pdf". This license can be also found at http://www.ltesimulator.com/
// The above file and the present reference are part of the software itself,
// and cannot be removed from it.
//


#include <math.h>
#include <assert.h>
#include "stack/phy/layer/LtePhyUeMode4D2D.h"
#include "stack/phy/packet/LteFeedbackPkt.h"
#include "stack/d2dModeSelection/D2DModeSelectionBase.h"
#include "stack/phy/packet/SpsCandidateResources.h"
#include "stack/phy/packet/cbr_m.h"

Define_Module(LtePhyUeMode4D2D);

LtePhyUeMode4D2D::LtePhyUeMode4D2D()
{
    handoverStarter_ = NULL;
    handoverTrigger_ = NULL;
}

LtePhyUeMode4D2D::~LtePhyUeMode4D2D()
{
}

void LtePhyUeMode4D2D::initialize(int stage)
{
    if (stage != inet::INITSTAGE_NETWORK_LAYER_2)
        LtePhyUe::initialize(stage);

    if (stage == inet::INITSTAGE_LOCAL)
    {
        adjacencyPSCCHPSSCH_ = par("adjacencyPSCCHPSSCH");
        pStep_ = par("pStep");
        selectionWindowStartingSubframe_ = par("selectionWindowStartingSubframe");
        numSubchannels_ = par("numSubchannels");
        subchannelSize_ = par("subchannelSize");
        d2dDecodingTimer_ = NULL;
        transmitting_ = false;
        currentCBR_= 0;
        cbrIndex_= -1; // Start at -1 simply to ensure that on first call to create subframe we start at index 0
        cbrHistory_.reserve(100);

        // The threshold has a size of 64, and allowable values of 0 - 66
        // Deciding on this for now as it makes the most sense (low priority for both then more likely to take it)
        // High priority for both then less likely to take it.
        for (int i = 1; i < 66; i++)
        {
            ThresPSSCHRSRPvector_.push_back(i);
        }
    }
    else if (stage == INITSTAGE_NETWORK_LAYER_2)
    {
        // Need to start initialising the sensingWindow
        deployer_ = getDeployer();
        int index = intuniform(0, binder_->phyPisaData.maxChannel() - 1);
        deployer_->lambdaInit(nodeId_, index);
        deployer_->channelUpdate(nodeId_, intuniform(1, binder_->phyPisaData.maxChannel2()));

//        LteMacBase* mac = binder_->getMacFromMacNodeId(nodeId_);
        allocator_ = new LteAllocationModule(mac_, D2D);
        allocator_->initAndReset(deployer_->getNumRbUl(), deployer_->getNumBands());
        createSubframe(NOW);
    }
}

void LtePhyUeMode4D2D::handleSelfMessage(cMessage *msg)
{
    if (msg->isName("d2dDecodingTimer"))
    {
        while (!sciFrames_.empty()){
            // Get received SCI and it's corresponding RsrpVector
            LteAirFrame* frame = sciFrames_.back();
            std::vector<double> rsrpVector = sciRsrpVectors_.back();
            std::vector<double> rssiVector = sciRssiVectors_.back();

            // Remove it from the vector
            sciFrames_.pop_back();
            sciRsrpVectors_.pop_back();
            sciRssiVectors_.pop_back();

            UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(frame->removeControlInfo());

            // decode the selected frame
            decodeAirFrame(frame, lteInfo, rsrpVector, rssiVector);

            // TODO: log the cbr here and send to MAC layer
            currentCBR_ = currentCBR_/numSubchannels_;
            cbrHistory_[cbrIndex_]=currentCBR_;
            currentCBR_=0;
            updateCBR();
        }
        while (!tbFrames_.empty())
        {
            LteAirFrame* frame = tbFrames_.back();
            std::vector<double> rsrpVector = tbRsrpVectors_.back();
            std::vector<double> rssiVector = tbRssiVectors_.back();

            tbFrames_.pop_back();
            tbRsrpVectors_.pop_back();
            tbRssiVectors_.pop_back();

            UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(frame->removeControlInfo());

            // decode the selected frame
            decodeAirFrame(frame, lteInfo, rsrpVector, rssiVector);
        }
        delete msg;
        d2dDecodingTimer_ = NULL;
    }
    else if (msg->isName("createSubframe"))
    {
        cbrIndex_++;
        if (cbrIndex_ == 100)
            cbrIndex_ = 0;
        cbrHistory_[cbrIndex_] = currentCBR_;
        createSubframe(NOW);
    }
    else
        LtePhyUe::handleSelfMessage(msg);
}

// TODO: ***reorganize*** method
void LtePhyUeMode4D2D::handleAirFrame(cMessage* msg)
{
    UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(msg->removeControlInfo());

    if (useBattery_)
    {
        //TODO BatteryAccess::drawCurrent(rxAmount_, 0);
    }
    connectedNodeId_ = masterId_;
    LteAirFrame* frame = check_and_cast<LteAirFrame*>(msg);
    EV << "LtePhyUeMode4D2D: received new LteAirFrame with ID " << frame->getId() << " from channel" << endl;
    //Update coordinates of this user
    if (lteInfo->getFrameType() == HANDOVERPKT)
    {
        // check if handover is already in process
        if (handoverTrigger_ != NULL && handoverTrigger_->isScheduled())
        {
            delete lteInfo;
            delete frame;
            return;
        }

        handoverHandler(frame, lteInfo);
        return;
    }

//    // Check if the frame is for us ( MacNodeId matches or - if this is a multicast communication - enrolled in multicast group)
//    if (lteInfo->getDestId() != nodeId_ && !(binder_->isInMulticastGroup(nodeId_, lteInfo->getMulticastGroupId())))
//    {
//        EV << "ERROR: Frame is not for us. Delete it." << endl;
//        EV << "Packet Type: " << phyFrameTypeToA((LtePhyFrameType)lteInfo->getFrameType()) << endl;
//        EV << "Frame MacNodeId: " << lteInfo->getDestId() << endl;
//        EV << "Local MacNodeId: " << nodeId_ << endl;
//        delete lteInfo;
//        delete frame;
//        return;
//    }

//    if (binder_->isInMulticastGroup(nodeId_,lteInfo->getMulticastGroupId()))
//    {
    // HACK: if this is a multicast connection, change the destId of the airframe so that upper layers can handle it
    // All packets in mode 4 are multicast so I feel like the effort to get this working is pointless, I can't easily determine the issue
    lteInfo->setDestId(nodeId_);
//    }

    // send H-ARQ feedback up
    if (lteInfo->getFrameType() == HARQPKT || lteInfo->getFrameType() == GRANTPKT || lteInfo->getFrameType() == RACPKT || lteInfo->getFrameType() == D2DMODESWITCHPKT)
    {
        handleControlMsg(frame, lteInfo);
        return;
    }

    // this is a DATA packet

    // if not already started, auto-send a message to signal the presence of data to be decoded
    if (d2dDecodingTimer_ == NULL)
    {
        d2dDecodingTimer_ = new cMessage("d2dDecodingTimer");
        d2dDecodingTimer_->setSchedulingPriority(10);          // last thing to be performed in this TTI
        scheduleAt(NOW, d2dDecodingTimer_);
    }

    // store frame, together with related control info
    frame->setControlInfo(lteInfo);

    // Capture the Airframe for decoding later
    storeAirFrame(frame);
}

void LtePhyUeMode4D2D::handleUpperMessage(cMessage* msg)
{
//    if (useBattery_) {
//    TODO     BatteryAccess::drawCurrent(txAmount_, 1);
//    }

    UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(msg->removeControlInfo());

    if (lteInfo->getFrameType() == GRANTPKT)
    {
        // Generate CSRs or save the grant for use when generating SCI information
        LteMode4SchedulingGrant* grant = check_and_cast<LteMode4SchedulingGrant*>(msg);
        if (grant->getTotalGrantedBlocks() == 0){
            // Generate a vector of CSRs and send it to the MAC layer
            computeCSRs(grant);
            delete grant;
        }
        else
        {
            sciGrant_ = grant;
        }
        // No need to go any further.
        return;
    }

    // Store the RBs used for transmission. For interference computation
    RbMap rbMap = lteInfo->getGrantedBlocks();
    UsedRBs info;
    info.time_ = NOW;
    info.rbMap_ = rbMap;

    usedRbs_.push_back(info);

    std::vector<UsedRBs>::iterator it = usedRbs_.begin();
    while (it != usedRbs_.end())  // purge old allocations
    {
        if (it->time_ < NOW - 0.002)
            usedRbs_.erase(it++);
        else
            ++it;
    }
    lastActive_ = NOW;

    // TODO: Find a better place to put this.
    UserControlInfo* SCIInfo = lteInfo->dup();
    LteAirFrame* frame = NULL;
    if (lteInfo->getFrameType() == DATAPKT && lteInfo->getUserTxParams() != NULL)
    {
        // This realistically isn't important as we won't be using cqi in the end.
//        double cqi = lteInfo->getUserTxParams()->readCqiVector()[lteInfo->getCw()];
//        if (lteInfo->getDirection() == UL)
//            emit(averageCqiUl_, cqi);
//        else if (lteInfo->getDirection() == D2D || D2D_MULTI)
//            emit(averageCqiD2D_, cqi);

        RbMap sciRbs;
        if (adjacencyPSCCHPSSCH_)
        {
            // Setup so SCI gets 2 RBs from the grantedBlocks.
            RbMap::iterator it;
            std::map<Band, unsigned int>::iterator jt;
            //for each Remote unit used to transmit the packet
            int allocatedBlocks = 0;
            for (it = rbMap.begin(); it != rbMap.end(); ++it)
            {
                if (allocatedBlocks == 2)
                {
                    break;
                }
                //for each logical band used to transmit the packet
                for (jt = it->second.begin(); jt != it->second.end(); ++jt)
                {
                    Band band = jt->first;

                    if (allocatedBlocks == 2)
                    {
                        // Have all the blocks allocated to the SCI so can move on.
                        break;
                    }

                    if (jt->second == 0) // this Rb is not allocated
                        continue;
                    else
                    {
                        // RB is allocated to grant, now give it to the SCI.
                        if (jt->second == 1){
                            jt->second = 0;
                            sciRbs[it->first][band] = 1;
                            ++allocatedBlocks;
                        }
                    }
                }
            }
        }
        else
        {
            // Setup so SCI gets 2 RBs from the grantedBlocks.
            RbMap::iterator it;
            std::map<Band, unsigned int>::iterator jt;
            int allocatedRbs = 0;
            //for each Remote unit used to transmit the packet
            for (it = rbMap.begin(); it != rbMap.end(); ++it)
            {
                if (allocatedRbs == 2)
                {
                    break;
                }
                //for each logical band used to transmit the packet
                for (jt = it->second.begin(); jt != it->second.end(); ++jt)
                {
                    if (allocatedRbs == 2)
                    {
                        // Have all the blocks allocated to the SCI so can move on.
                        break;
                    }
                    // sciRbs[remote][band] = assigned Rb
                    sciRbs[it->first][jt->first] = 1;
                    ++allocatedRbs;
                }
            }
        }

        SCIInfo->setFrameType(SCIPKT);
        SCIInfo->setGrantedBlocks(sciRbs);

        /*
         * Need to prepare the airframe were sending
         * Ensure that it will fit into it's grant
         * if not don't send anything except a break reservation message
         */
        EV << NOW << " LtePhyUeMode4D2D::handleUpperMessage - message from stack" << endl;

        frame = prepareAirFrame(msg, lteInfo);

        // Going to assume that it won't be the case that we have this issue
//        cPacket* pkt = check_and_cast<cPacket*>(msg);
//        if (pkt->getBitLength() > sciGrant_->getGrantedCwBytes(0))
//        {
//            cMessage* grantBreak = new cMessage("GRANTBREAK");
//            send(grantBreak, upperGateOut_);
//
//            // At this point we need not go any further
//            return;
//        }

        // create LteAirFrame and encapsulate the received packet
        SidelinkControlInformation* SCI = createSCIMessage();
        LteAirFrame* sciFrame = prepareAirFrame(SCI, SCIInfo);
        // TODO: Set the MCS for the sciFrame to 0 for sending.

        // TODO: Signal for Sending SCI
        sendBroadcast(sciFrame);
    }
    else if (lteInfo->getFrameType() == HARQPKT)
    {
        frame = new LteAirFrame("harqFeedback-grant");
    }
    else
    {
        return;
    }

    // if this is a multicast/broadcast connection, send the frame to all neighbors in the hearing range
    // otherwise, send unicast to the destination

    EV << "LtePhyUeMode4D2D::handleUpperMessage - " << nodeTypeToA(nodeType_) << " with id " << nodeId_
           << " sending message to the air channel. Dest=" << lteInfo->getDestId() << endl;

    // Mark that we are in the process of transmitting a packet therefore when we go to decode messages we can mark as failure due to half duplex
    transmitting_=true;

    if (lteInfo->getDirection() == D2D_MULTI)
        sendBroadcast(frame);
    else
        sendUnicast(frame);

    // TODO: Mark the current subframe as unsensed due to this sending.
}

void LtePhyUeMode4D2D::computeCSRs(LteMode4SchedulingGrant* &grant)
{
    EV << NOW << " LtePhyUeMode4D2D::computeCSRs - going through sensing window to compute CSRS..." << endl;
    // Determine the total number of possible CSRs
    int totalPossibleCSRs = (grant->getMaximumLatency() - selectionWindowStartingSubframe_) * numSubchannels_;
    simtime_t startTime = (NOW + TTI * selectionWindowStartingSubframe_);

    simtime_t endTime = (NOW + TTI * 100);

    if (grant->getMaximumLatency() < 100)
    {
        endTime = (NOW + TTI * grant->getMaximumLatency());
    }

    int subframeIndex = sensingWindow_.size() + selectionWindowStartingSubframe_;
    std::vector<std::vector<Subchannel*>> selectionWindow;
    for (simtime_t t = startTime; t <= endTime ; t+=TTI)
    {
        std::vector<Subchannel*> subframe;
        for (int i=0;i<numSubchannels_;i++)
        {
            Subchannel* subchannel = new Subchannel(subchannelSize_, t, subframeIndex, i);
            // TODO: Need to add the bands to the individual subchannels as well.
            std::vector<Band> allocatedBands;
            for (Band b = i * subchannelSize_; b < (i * subchannelSize_) + subchannelSize_ ; b++)
            {
                allocatedBands.push_back(b);
            }
            subchannel->setOccupiedBands(allocatedBands);
            subframe.push_back(subchannel);
        }
        selectionWindow.insert(selectionWindow.begin(), subframe);
        ++subframeIndex;
    }

    int thresholdIncreaseFactor = 0;
    std::vector<std::vector<Subchannel*>> possibleCSRs;
    bool checkedSensed = false;

    // Cannot continue until possibleCSRs is at least 20% of all CSRs
    while (possibleCSRs.size() < (totalPossibleCSRs * .2))
    {
        if(!checkedSensed)
        {
            checkSensed(selectionWindow, grant);
            checkedSensed = true;
        }
        checkRSRP(selectionWindow, grant, thresholdIncreaseFactor);
        possibleCSRs = getPossibleCSRs(selectionWindow, grant);
        ++thresholdIncreaseFactor;
    }

    /*
     * Using RSSI pick subchannels with lowest RSSI (Across time) pick 20% lowest.
     * report this to MAC layer.
     */
    std::vector<std::vector<Subchannel*>> optimalCSRs;

    if (possibleCSRs.size() > (totalPossibleCSRs * .2))
    {
        optimalCSRs = selectBestRSSIs(possibleCSRs, grant, totalPossibleCSRs);
    }
    else
    {
        optimalCSRs = possibleCSRs;
    }

    // Send the packet up to the MAC layer where it will choose the CSR and the retransmission if that is specified
    // Need to generate the message that is to be sent to the upper layers.
    SpsCandidateResources* candidateResourcesMessage = new SpsCandidateResources("CSRs");
    candidateResourcesMessage->setCSRs(optimalCSRs);
    send(candidateResourcesMessage, upperGateOut_);
}

void LtePhyUeMode4D2D::checkSensed(std::vector<std::vector<Subchannel*>> &selectionWindow, LteMode4SchedulingGrant* &grant)
{
    EV << NOW << " LtePhyUeMode4D2D::checkSensed - eliminating CSRS which were not sensed in sensing window selectionWindow..." << endl;
    int pRsvpTx = grant->getPeriod();
    unsigned int grantLength = grant->getNumSubchannels();
    int cResel = grant->getResourceReselectionCounter();
    std::vector<double> allowedRRIs = grant->getPossibleRRIs();
    for (int i=0; i < selectionWindow.size();i++)
    {
        int adjustedIndex = i+1000+selectionWindowStartingSubframe_;
        bool noUnsensed;
        for(int j=0; j<numSubchannels_;j++)
        {
            if (j+grantLength > numSubchannels_)
            {
                // We cannot fill this grant in this subframe and should move to the next one
                break;
            }
            // Check if the current subframe was sensed

            // Check that this frame is allowed to be used i.e. all the previous corresponding frames are sensed.
            int pRsvpTxPrime = pStep_ * pRsvpTx / 100;
            int Q = 1;
            // Need to calculate which subframes are not allowed.
            std::vector<double>::iterator kt;
            for(kt=allowedRRIs.begin(); kt!=allowedRRIs.end(); kt++)
            {
                // This applies to all allowed RRIs as well.
                if ((*kt) < 1 && 10*pStep_ - i < pStep_ * (*kt))
                {
                    Q = 1/ *kt;
                }
                for(int z=0; z<sensingWindow_.size(); z++)
                {
                    noUnsensed = true;
                    // Go through through the frames in the sensing window, check if they are sensed
                    if (!sensingWindow_[z][0]->getSensed())
                    {
                        int q=1;
                        // If the first subchannel is not sensed then the subframe is not sensed either and as such the subchannels can be removed
                        while (q<Q && noUnsensed)
                        {
                            int resel=0;
                            while (resel<cResel && noUnsensed)
                            {
                                if (adjustedIndex + resel * pRsvpTxPrime == z + pStep_ * (*kt) * q)
                                {
                                    selectionWindow[i][0]->setSensed(false);
                                    selectionWindow[i][0]->setPossibleCSR(false);
                                    // Allows us to move onto the next subframe and ignore this one as it isn't one that can be selected.
                                    noUnsensed=false;
                                }
                                ++resel;
                            }
                            ++q;
                        }
                    }
                }
            }
        }
    }
}

void LtePhyUeMode4D2D::checkRSRP(std::vector<std::vector<Subchannel*>> &selectionWindow, LteMode4SchedulingGrant* &grant, int thresholdIncreaseFactor)
{
    EV << NOW << " LtePhyUeMode4D2D::checkRSRP - checking selectionWindow and filtering CSRs based on RSRP..." << endl;
    std::vector<double> averageRSRPs;
    std::vector<int> priorities;
    std::map<int, std::vector<int>> dissallowedCSRs;
    std::vector<SidelinkControlInformation*> scis;
    bool subchannelReserved = false;
    int grantLength = grant->getNumSubchannels();
    int cResel = grant->getResourceReselectionCounter() * 10;
    int pRsvpTx = grant->getPeriod();
    int pRsvpTxPrime = pStep_ * pRsvpTx / 100;

    for (int i=0; i < sensingWindow_.size();i++)
    {
        if (sensingWindow_[i][0]->getSensed())
        {
            for(int j=0; j<numSubchannels_;j++)
            {
                if (j+grantLength > numSubchannels_)
                {
                    // We cannot fill this grant in this subframe and should move to the next one
                    break;
                }

                for (int k=j; k<j+grantLength; k++)
                {
                    if (sensingWindow_[i][k]->getReserved())
                    {
                        subchannelReserved = true;
                        SidelinkControlInformation* receivedSCI = check_and_cast<SidelinkControlInformation*>(sensingWindow_[i][k]->getSCIMessage());
                        UserControlInfo* sciInfo = check_and_cast<UserControlInfo*>(receivedSCI->getControlInfo());
                        averageRSRPs.push_back(sensingWindow_[i][k]->getAverageRSRP());
                        priorities.push_back(receivedSCI->getPriority());
                        scis.push_back(receivedSCI);

                        std::tuple<int, int> indexAndLength = decodeRivValue(receivedSCI, sciInfo);
                        int lengthInSubchannels = std::get<1>(indexAndLength);
                        k += lengthInSubchannels;
                    }
                }
                if (subchannelReserved)
                {
                    subchannelReserved = false;

                    // Get the priorities of both messages
                    int messagePriority = grant->getSpsPriority();
                    for (int l=0; l<averageRSRPs.size(); l++)
                    {
                        double averageRSRP = averageRSRPs[l];
                        int receivedPriority = priorities[l];
                        SidelinkControlInformation* receivedSCI = scis[l];
                        // Get the threshold for the corresponding priorities
                        int index = messagePriority * 8 + receivedPriority + 1;
                        int threshold = ThresPSSCHRSRPvector_[index];
                        int thresholdDbm = (-128 * (threshold-1)*2) + (3 * thresholdIncreaseFactor);



                        if (averageRSRP > thresholdDbm)
                        {
                            // This series of subchannels is to be excluded
                            int Q = 1;
                            unsigned int rri = receivedSCI->getResourceReservationInterval();
                            if (rri < 1 && i <= (pStep_ * 10) - pStep_ * rri)
                            {
                                Q = 1/rri;
                            }

                            for (int q = 1; q <= Q; q++)
                            {
                                for (int c = 0; c < cResel; c++)
                                {
                                    int dissallowedIndex = i + q * pStep_ * rri -c * pRsvpTxPrime;
                                    for (int k=j; k<j+grantLength; k++){
                                        dissallowedCSRs[dissallowedIndex].push_back(k);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Go through the dissallowed indices and set each subchannel if it is dissallowed.
    std::map<int, vector<int>>::iterator it;
    int maxLatency = 100;
    if (grant->getMaximumLatency() < 100)
    {
        maxLatency = grant->getMaximumLatency();
    }

    for (it=dissallowedCSRs.begin(); it!=dissallowedCSRs.end(); it++)
    {
        if (it->first >= 1000 + selectionWindowStartingSubframe_ && it->first <= 1000 + maxLatency)
        {
            std::vector<int>::iterator jt;
            for (jt=it->second.begin(); jt!=it->second.end(); jt++)
            {
                selectionWindow[it->first - (1000 + selectionWindowStartingSubframe_)][*jt]->setPossibleCSR(false);
            }
        }
    }
}

std::vector<std::vector<Subchannel*>> LtePhyUeMode4D2D::getPossibleCSRs(std::vector<std::vector<Subchannel*>> &selectionWindow, LteMode4SchedulingGrant* &grant)
{
    // Go through the selection window and determine the number of possible CSRs available
    EV << NOW << " LtePhyUeMode4D2D::getPossibleCSRs - Getting possible CSRs from from selectionWindow..." << endl;
    int grantLength = grant->getNumSubchannels();
    std::vector<std::vector<Subchannel*>> possibleCSRs;
    for (int i=0; i < selectionWindow.size(); i++)
    {
        if (selectionWindow[i][0]->getSensed())
        {
            int j=0;
            while (j < numSubchannels_)
            {
                if (j + grantLength > numSubchannels_)
                {
                    // Cannot fit the CSR therefore move on.
                    break;
                }
                std::vector<Subchannel*> possibleCSR;
                for (int k=j; k < j+grantLength; k++)
                {
                    if (!selectionWindow[i][k]->getPossibleCSR())
                    {
                        // This range of subchannels will not fit the CSR
                        j = k + 1;
                        break;
                    }
                    possibleCSR.push_back(selectionWindow[i][k]);
                }
                possibleCSRs.push_back(possibleCSR);
                ++j;
            }
        }
    }
    return possibleCSRs;
}

std::vector<std::vector<Subchannel*>> LtePhyUeMode4D2D::selectBestRSSIs(std::vector<std::vector<Subchannel*>> &possibleCSRs, LteMode4SchedulingGrant* &grant, int totalPossibleCSRs)
{
    EV << NOW << " LtePhyUeMode4D2D::selectBestRSSIs - Selecting best CSRs from possible CSRs..." << endl;
    int decrease = pStep_;
    if (grant->getPeriod() < 100)
    {
        // Same as pPrimeRsvpTx from other parts of the function
        decrease = (pStep_ * grant->getPeriod())/100;
    }

    std::vector<std::vector<Subchannel*>> optimalCSRs;
    std::vector<std::vector<Subchannel*>>::iterator it;

    while(optimalCSRs.size() < (totalPossibleCSRs * 0.2)){
        // I feel like there is definitely a better approach (going through once makes the most sense)
        // But based on the wording of the text this is what they describe.
        // But TODO: Fix this to be more efficient.
        int minRSSI = 0;
        int currentMinCSR = 0;
        for (it=possibleCSRs.begin(); it != possibleCSRs.end(); it++)
        {
            Subchannel* initialSubchannel = *it->begin();
            Subchannel* finalSubchannel = it->back();

            int subframe = initialSubchannel->getSubframeIndex();
            int startingSubchannelIndex = initialSubchannel->getSubchannelIndex();
            int finalSubchannelIndex = finalSubchannel->getSubchannelIndex();

            subframe -= decrease;
            int totalRSSI = 0;
            int numSubchannels = 0;
            while (subframe > 0 && subframe < sensingWindow_.size())
            {
                for (int subchannelCounter = startingSubchannelIndex; subchannelCounter <= finalSubchannelIndex; subchannelCounter++)
                {
                    if (sensingWindow_[subframe][subchannelCounter]->getSensed())
                    {
                        totalRSSI += sensingWindow_[subframe][subchannelCounter]->getAverageRSSI();
                        ++numSubchannels;
                    }
                    else
                    {
                        // Subframe wasn't sensed so skip to the next one.
                        break;
                    }
                }
                subframe -= decrease;
            }
            int averageRSSI = 0;
            if (numSubchannels != 0)
            {
                // Can be the case when the sensing window is not full that we don't find the historic CSRs
                averageRSSI = totalRSSI / numSubchannels;
            }
            if (averageRSSI < minRSSI || minRSSI == 0)
            {
                minRSSI = averageRSSI;
                currentMinCSR = it - possibleCSRs.begin();
            }
        }
        optimalCSRs.push_back(possibleCSRs[currentMinCSR]);
        possibleCSRs.erase(possibleCSRs.begin()+currentMinCSR);
        minRSSI = 0;
    }
    return optimalCSRs;
}

SidelinkControlInformation* LtePhyUeMode4D2D::createSCIMessage()
{
    EV << NOW << " LtePhyUeMode4D2D::createSCIMessage - Start creating SCI..." << endl;

    SidelinkControlInformation* sci = new SidelinkControlInformation("SCI Message");

    /*
     * Priority (based on upper layer)
     * 0-7
     * Mapping unknown, so possibly just take the priority max and min and map to 0-7
     * This needs to be integrated from the application layer.
     * Will take this from the scheduling grant.
     */
    sci->setPriority(sciGrant_->getSpsPriority());

    /* Resource Interval
     *
     * 0 -> 16
     * 0 = not reserved
     * 1 = 100ms (1) RRI [Default]
     * 2 = 200ms (2) RRI
     * ...
     * 10 = 1000ms (10) RRI
     * 11 = 50ms (0.5) RRI
     * 12 = 20ms (0.2) RRI
     * 13 - 15 = Reserved
     *
     * TODO: Make sure the period is set to the correct level (i.e. period = 100/200/300 etc)
     */
    if (sciGrant_->getExpiration() != 0)
    {
        sci->setResourceReservationInterval(sciGrant_->getPeriod()/100);
    }
    else
    {
        sci->setResourceReservationInterval(0);
    }
    /* frequency Resource Location
     * Based on another parameter RIV
     * but size is
     * Log2(Nsubchannel(Nsubchannel+1)/2) (rounded up)
     * 0 - 8 bits
     * 0 - 256 different values
     *
     * Based on TS36.213 14.1.1.4C
     *     if SubchannelLength -1 < numSubchannels/2
     *         RIV = numSubchannels(SubchannelLength-1) + subchannelIndex
     *     else
     *         RIV = numSubchannels(numSubchannels-SubchannelLength+1) + (numSubchannels-1-subchannelIndex)
     */
    unsigned int riv;
    //
    if (sciGrant_->getNumSubchannels() -1 <= (numSubchannels_/2))
    {
        // RIV calculation for less than half+1
        riv = ((numSubchannels_ * (sciGrant_->getNumSubchannels() - 1)) + sciGrant_->getStartingSubchannel());
    }
    else
    {
        // RIV calculation for less than half size
        riv = ((numSubchannels_ * (numSubchannels_ - sciGrant_->getNumSubchannels() + 1)) + (numSubchannels_ - 1 - sciGrant_->getStartingSubchannel()));
    }

    sci->setFrequencyResourceLocation(riv);

    /* TimeGapRetrans
     * 1 - 15
     * ms from init to retrans
     */
    sci->setTimeGapRetrans(sciGrant_->getTimeGapTransRetrans());


    /* mcs
     * 5 bits
     * 26 combos
     * TODO: where do we find this guy
     * Technically the MAC layer determines the MCS that it wants the message sent with and as such it will be in the packet
     */
    sci->setMcs(sciGrant_->getMcs());

    /* retransmissionIndex
     * 0 / 1
     * if 0 retrans is in future/this is the first transmission
     * if 1 retrans is in the past/this is the retrans
     */
    if (sciGrant_->getRetransmission())
    {
        sci->setRetransmissionIndex(1);
    }
    else
    {
        sci->setRetransmissionIndex(0);
    }

    /* Filler up to 32 bits
     * Can just set the length to 32 bits and ignore the issue of adding filler
     */
    sci->setBitLength(32);

    return sci;
}

LteAirFrame* LtePhyUeMode4D2D::prepareAirFrame(cMessage* msg, UserControlInfo* lteInfo){
    // Helper function to prepare airframe for sending.
    LteAirFrame* frame = new LteAirFrame("airframe");

    frame->encapsulate(check_and_cast<cPacket*>(msg));
    frame->setSchedulingPriority(airFramePriority_);
    frame->setDuration(TTI);

    lteInfo->setCoord(getRadioPosition());

    lteInfo->setTxPower(txPower_);
    lteInfo->setD2dTxPower(d2dTxPower_);
    frame->setControlInfo(lteInfo);

    return frame;
}

void LtePhyUeMode4D2D::storeAirFrame(LteAirFrame* newFrame)
{
    // implements the capture effect
    // store the frame received from the nearest transmitter
    UserControlInfo* newInfo = check_and_cast<UserControlInfo*>(newFrame->getControlInfo());
    Coord myCoord = getCoord();

    std::vector<double> rsrpVector = channelModel_->getRSRP_D2D(newFrame, newInfo, nodeId_, myCoord);
    // TODO: Seems we don't really actually need the enbId, I have set it to 0 as it is referenced but never used for calc
    std::vector<double> rssiVector = channelModel_->getSINR_D2D(newFrame, newInfo, nodeId_, myCoord, 0, rsrpVector);

    // TODO: Update the subchannel associated with this transmission to include the average RSRP for the sub channel
    // Need to be able to figure out which subchannel is associated to the Rbs in this case
    if (newInfo->getFrameType() == SCIPKT){
        sciFrames_.push_back(newFrame);
        sciRsrpVectors_.push_back(rsrpVector);
        sciRssiVectors_.push_back(rssiVector);
    }
    else{
        tbFrames_.push_back(newFrame);
        tbRsrpVectors_.push_back(rsrpVector);
        tbRssiVectors_.push_back(rssiVector);
    }
}

void LtePhyUeMode4D2D::decodeAirFrame(LteAirFrame* frame, UserControlInfo* lteInfo, std::vector<double> &rsrpVector, std::vector<double> &rssiVector)
{
    EV << NOW << " LtePhyUeMode4D2D::decodeAirFrame - Start decoding..." << endl;

    // apply decider to received packet
    bool result = true;

    RemoteSet r = lteInfo->getUserTxParams()->readAntennaSet();
    if (r.size() > 1)
    {
        // DAS
        for (RemoteSet::iterator it = r.begin(); it != r.end(); it++)
        {
            EV << "LtePhyUeMode4D2D::decodeAirFrame: Receiving Packet from antenna " << (*it) << "\n";

            /*
             * On UE set the sender position
             * and tx power to the sender das antenna
             */

            // cc->updateHostPosition(myHostRef,das_->getAntennaCoord(*it));
            // Set position of sender
            // Move m;
            // m.setStart(das_->getAntennaCoord(*it));
            RemoteUnitPhyData data;
            data.txPower=lteInfo->getTxPower();
            data.m=getRadioPosition();
            frame->addRemoteUnitPhyDataVector(data);
        }
        // apply analog models For DAS
        result=channelModel_->errorDas(frame,lteInfo);
    }

    cPacket* pkt = frame->decapsulate();

//    // attach the decider result to the packet as control info
//    lteInfo->setDeciderResult(result);
//    pkt->setControlInfo(lteInfo);

    if(lteInfo->getFrameType() == SCIPKT)
    {
        //RELAY and NORMAL
        if (lteInfo->getDirection() == D2D_MULTI)
            result = channelModel_->error_Mode4_D2D(frame,lteInfo,rsrpVector, 0);
        else
            result = channelModel_->error(frame,lteInfo);
        if (result)
        {
            // TODO: Signal successfully decoded SCI message
            SidelinkControlInformation* sci = check_and_cast<SidelinkControlInformation*>(pkt);
            std::tuple<int, int> indexAndLength = decodeRivValue(sci, lteInfo);
            int subchannelIndex = std::get<0>(indexAndLength);
            int lengthInSubchannels = std::get<1>(indexAndLength);

            currentCBR_ += lengthInSubchannels;

            std::vector<Subchannel*>::iterator kt;
            std::vector<Subchannel*> currentSubframe = sensingWindow_.back();
            for(kt=currentSubframe.begin()+subchannelIndex; kt!=currentSubframe.begin()+subchannelIndex+lengthInSubchannels; kt++)
            {
                // Record the SCI in the subchannel.
                (*kt)->setSCI(sci->dup());
            }
            lteInfo->setDeciderResult(true);
            pkt->setControlInfo(lteInfo);
            decodedScis_.push_back(pkt);
        }
        else
        {
            // TODO: Signal failed to decode the SCI message
        }
        // We do not want to send SCIs to the upper layers, as such return now.
        return;
    }
    else
    {
        // Have a TB want to make sure we have the SCI for it.
        bool foundCorrespondingSci = false;
        SidelinkControlInformation* correspondingSCI;
        UserControlInfo* sciInfo;
        std::vector<cPacket*>::iterator it;
        for(it=decodedScis_.begin(); it!=decodedScis_.end(); it++)
        {
            sciInfo = check_and_cast<UserControlInfo*>((*it)->removeControlInfo());
            // if the SCI and TB have same source then we have the right SCI
            if (sciInfo->getSourceId() == lteInfo->getSourceId())
            {
                //Successfully received the SCI
                foundCorrespondingSci = true;

                correspondingSCI = check_and_cast<SidelinkControlInformation*>(*it);

                //RELAY and NORMAL
                if (lteInfo->getDirection() == D2D_MULTI)
                    result = channelModel_->error_Mode4_D2D(frame,lteInfo,rsrpVector, correspondingSCI->getMcs());
                else
                    result = channelModel_->error(frame,lteInfo);

                // Remove the SCI
                decodedScis_.erase(it);
                break;
            }
        }
        if(result && !foundCorrespondingSci)
        {
            // TODO: Signal failed to decode TB due to lack of sci
        }
        // TODO: Signal successfully found the SCI message
        else if (!result && foundCorrespondingSci)
        {
            //TODO: Failed to decode TB but decoded the SCI.
        }
        else if (!result && ! foundCorrespondingSci)
        {
            // TODO: Just outright fail
        }
        else
        {
            //TODO: Signal successfully decoded both the SCI and TB
            // Now need to find the associated Subchannels, record the RSRP and RSSI for the message and go from there.
            // Need to again do the RIV steps
            std::tuple<int, int> indexAndLength = decodeRivValue(correspondingSCI, sciInfo);
            int subchannelIndex = std::get<0>(indexAndLength);
            int lengthInSubchannels = std::get<1>(indexAndLength);

            std::vector<Subchannel*>::iterator kt;
            std::vector<Subchannel*> currentSubframe = sensingWindow_.back();
            for(kt=currentSubframe.begin()+subchannelIndex; kt!=currentSubframe.begin()+subchannelIndex+lengthInSubchannels; kt++)
            {
                std::vector<Band>::iterator lt;
                std::vector<Band> allocatedBands = (*kt)->getOccupiedBands();
                for (lt=allocatedBands.begin(); lt!=allocatedBands.end(); lt++)
                {
                    // Record RSRP and RSSI for this band
                    (*kt)->addRsrpValue(rsrpVector[(*lt)], (*lt));
                    (*kt)->addRssiValue(rssiVector[(*lt)], (*lt));
                }
            }
        }

        // send decapsulated message along with result control info to upperGateOut_
        lteInfo->setDeciderResult(false);
        pkt->setControlInfo(lteInfo);
        send(pkt, upperGateOut_);

        if (getEnvir()->isGUI())
            updateDisplayString();
    }

    // update statistics
    if (result)
        numAirFrameReceived_++;
    else
        numAirFrameNotReceived_++;

    EV << "Handled LteAirframe with ID " << frame->getId() << " with result "
       << ( result ? "RECEIVED" : "NOT RECEIVED" ) << endl;
}

std::tuple<int,int> LtePhyUeMode4D2D::decodeRivValue(SidelinkControlInformation* sci, UserControlInfo* sciInfo)
{
    EV << NOW << " LtePhyUeMode4D2D::decodeRivValue - Decoding RIV value of SCI allows correct placement in sensing window..." << endl;
    //UserControlInfo* lteInfo = check_and_cast<UserControlInfo*>(pkt->removeControlInfo());
    RbMap rbMap = sciInfo->getGrantedBlocks();
    RbMap::iterator it;
    std::map<Band, unsigned int>::iterator jt;
    Band startingBand;
    bool bandNotFound = true;

    it = rbMap.begin();

    while (it != rbMap.end() && bandNotFound )
    {
        for (jt = it->second.begin(); jt != it->second.end(); ++jt)
       {
           Band band = jt->first;
           if (jt->second == 1) // this Rb is not allocated
           {
               startingBand = band;
               bandNotFound= false;
               break;
           }
       }
    }

    // Get RIV first as this is common
    unsigned int RIV = sci->getFrequencyResourceLocation();

    // Get the subchannel Index (allows us later to calculate the length of the message
    int subchannelIndex;
    if (adjacencyPSCCHPSSCH_)
    {
        // If adjacent: Subchannel Index = band//subchannelSize
        subchannelIndex = startingBand/subchannelSize_;
    }
    else
    {
        // If non-adjacent: Subchannel Index = band/2
        subchannelIndex = startingBand/2;
    }

    // Based on TS36.213 14.1.1.4C
    // if SubchannelLength -1 < numSubchannels/2
    // RIV = numSubchannels(SubchannelLength-1) + subchannelIndex
    // else
    // RIV = numSubchannels(numSubchannels-SubchannelLength+1) + (numSubchannels-1-subchannelIndex)
    // RIV limit
    // log2(numSubchannels(numSubchannels+1)/2)
    double subchannelLOverHalf = (numSubchannels_ + 2 + (( -1 - subchannelIndex - RIV )/numSubchannels_));
    double subchannelLUnderHalf = (RIV + numSubchannels_ - subchannelIndex)/numSubchannels_;
    int lengthInSubchannels;

    // First the number has to be whole in both cases, it's length + subchannelIndex must be less than the number of subchannels
    if (floor(subchannelLOverHalf) == subchannelLOverHalf && subchannelLOverHalf <= numSubchannels_ && subchannelLOverHalf + subchannelIndex <= numSubchannels_)
    {
        lengthInSubchannels = subchannelLOverHalf;
    }
    // Same as above but also the length must be less than half + 1
    else if (floor(subchannelLUnderHalf) == subchannelLUnderHalf && subchannelLUnderHalf + subchannelIndex <= numSubchannels_ && subchannelLUnderHalf <= numSubchannels_ /2 + 1)
    {
        lengthInSubchannels = subchannelLUnderHalf;
    }
    return std::make_tuple(subchannelIndex, lengthInSubchannels);
}

void LtePhyUeMode4D2D::updateCBR()
{
    double cbr = 0;
    for (int i=0; i < cbrHistory_.size();i++)
    {
        cbr += cbrHistory_[i];
    }

    cbr = std::round(cbr);

    Cbr* cbrPkt = new Cbr("CBR");
    cbrPkt->setCbr(cbr);
    send(cbrPkt, upperGateOut_);
}

void LtePhyUeMode4D2D::createSubframe(simtime_t subframeTime)
{
    EV << NOW << " LtePhyUeMode4D2D::createSubframe - creating subframe to be added to sensingWindow..." << endl;
    std::vector<Subchannel*> subframe;

    Band band = 0;

    if (!adjacencyPSCCHPSSCH_)
    {
        // This assumes the bands only every have 1 Rb (which is fine as that appears to be the case)
        band = numSubchannels_*2;
    }
    for (int i = 0; i < numSubchannels_; i++)
    {
        Subchannel* currentSubchannel = new Subchannel(subchannelSize_, subframeTime);
        // Need to determine the RSRP and RSSI that corresponds to background noise
        // Best off implementing this in the channel model as a method.

        std::vector<Band> occupiedBands;

        int overallCapacity = 0;
        // Ensure the subchannel is allocated the correct number of RBs
        while(overallCapacity < subchannelSize_ && band < getBinder()->getNumBands())
        {
            // This acts like there are multiple RBs per band which is not allowed.
            overallCapacity += allocator_->getAllocatedBlocks(MAIN_PLANE, MACRO, band);
            occupiedBands.push_back(band);
            ++band;
        }
        currentSubchannel->setOccupiedBands(occupiedBands);
        subframe.push_back(currentSubchannel);
    }
    if (sensingWindow_.size() < 10*pStep_)
    {
        sensingWindow_.push_back(subframe);
    }
    else
    {
        sensingWindow_.erase(sensingWindow_.begin());
        sensingWindow_.push_back(subframe);
    }

    // Send self message to trigger another subframes creation and insertion. Need one for every TTI
    cMessage* createSubframe = new cMessage("createSubframe");
    createSubframe->setSchedulingPriority(0);        // Generate the subframe at start of next TTI
    scheduleAt(NOW + TTI, createSubframe);
}

void LtePhyUeMode4D2D::finish()
{
    if (getSimulation()->getSimulationStage() != CTX_FINISH)
    {
        // do this only at deletion of the module during the simulation
        //LtePhyUe::finish();
        LteAmc *amc = getAmcModule(masterId_);
        if (amc != NULL)
        {
            amc->detachUser(nodeId_, UL);
            amc->detachUser(nodeId_, DL);
            amc->detachUser(nodeId_, D2D);
        }

        // binder call
        binder_->unregisterNextHop(masterId_, nodeId_);

        // deployer call
        deployer_->detachUser(nodeId_);
    }
}
