//
//  scOutputDemand.h
//  ofxOceanodeSuperCollider
//
//  Whether an analyzer's per-frame work has a reader: its own window or
//  widget was drawn in the last frames (markShown() from the draw path; update
//  runs before draw, hence the 2-frame tolerance), or one of its outputs is
//  read elsewhere -- a cable, a scope window, a folded-node / inspector scope
//  (while folded / selected) or a Custom GUI binding. Cables are checked every frame; the other readers
//  walk container tables, so they are sampled every 15 frames.
//

#pragma once

#include "ofxOceanodeNodeModel.h"
#include "ofxOceanodeContainer.h"
#include "ofxOceanodeNode.h"
#include "ofxOceanodeNodeGui.h"
#include <memory>
#include <vector>

class scOutputDemand {
public:
    void watch(std::shared_ptr<ofxOceanodeAbstractParameter> p) { if(p) outputs.push_back(std::move(p)); }
    void clearOutputs() { outputs.clear(); }   // capacity kept: re-watch per frame
    void markShown() { lastShown = ofGetFrameNum(); }
    bool shown() const { return ofGetFrameNum() <= lastShown + 2; }

    bool needed(ofxOceanodeNodeModel& model) {
        return shown() || outputRead(model);
    }

    // Only the outputs' readers (not the node's own window or widget)
    bool outputRead(ofxOceanodeNodeModel& model) {
        const uint64_t frame = ofGetFrameNum();
        for(auto& p : outputs) if(p->hasOutConnections()) return true;
        if(frame >= nextSlowCheck) {
            nextSlowCheck = frame + 15;
            otherReader = readElsewhere(model);
        }
        return otherReader;
    }

private:
    std::vector<std::shared_ptr<ofxOceanodeAbstractParameter>> outputs;
    uint64_t lastShown = 0;
    uint64_t nextSlowCheck = 0;
    bool otherReader = true;

    bool readElsewhere(ofxOceanodeNodeModel& model) {
        ofxOceanodeContainer* container = model.getHostContainer();
        for(auto& p : outputs) {
            if(p->isScoped() || p->isTimelined()) return true;
            if(!container) return true;   // unknown: keep working
            if(container->customGuiContainsParameterAnywhere(*p)) return true;
            // Outputs are scoped-when-folded by default: that scope is drawn
            // while the node is folded, and in the inspector while selected
            ofxOceanodeNode* node = container->getNodeFromParameter(*p);
            if(node && node->isParameterScopedWhenFolded(*p)
               && (!node->getNodeGui().getExpanded() || node->getNodeGui().getSelected())) return true;
        }
        return false;
    }
};
