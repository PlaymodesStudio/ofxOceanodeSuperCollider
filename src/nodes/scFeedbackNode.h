//
//  scFeedbackNode.h
//  ofxOceanodeSuperCollider
//

#ifndef scFeedbackNode_h
#define scFeedbackNode_h

#include "../scSynthdef.h"

// Explicitly terminates graph-order dependency traversal at its input. The
// audio connection remains intact and the matching SynthDef reads it with
// InFeedback, so it receives the previous server block when its writer is
// later in the node order.
class scFeedbackNode : public scSynthdef {
public:
    explicit scFeedbackNode(synthdefDesc description)
    : scSynthdef(description) {}

    bool appendOrderedNodes(vector<scNode*> &nodesList,
                            map<scNode*, std::pair<int, vector<int>>>&,
                            vector<scNode*>) override {
        // Deliberately do not traverse availableInputs. The bus connection is
        // still assigned normally, but it does not impose an execution-order
        // dependency because the SynthDef reads it with InFeedback.
        if(std::find(nodesList.begin(), nodesList.end(), this) == nodesList.end()){
            nodesList.push_back(this);
            return true;
        }
        return false;
    }
};

#endif /* scFeedbackNode_h */
