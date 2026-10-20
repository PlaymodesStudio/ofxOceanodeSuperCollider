#include "scAudioRouting.h"
#include <cassert>
#include <iostream>
#include <vector>

struct Node;
struct Port {
    Node* node = nullptr;
    int index = -1;
    Node* getNodeRef() const { return node; }
};
struct Node {
    bool on = true;
    std::vector<Port> inputs;
};

static Port resolve(Port port){
    return scResolveAudioSource(port,
        [](Node* node){ return node->on; },
        [](Node* node){ return node->inputs.empty() ? Port{} : node->inputs.front(); });
}

static void expect(Port port, Node* node, int index = -1){
    const auto result = resolve(port);
    assert(result.node == node);
    assert(result.index == index);
}

int main(){
    Node oscillator, sidechain;
    Node filter{true, {{&oscillator, 2}, {&sidechain, 0}}};
    Node reverb{true, {{&filter, 1}}};

    expect({&reverb, 0}, &reverb, 0);
    reverb.on = false;
    expect({&reverb, 0}, &filter, 1);
    filter.on = false;
    // Consecutive Off nodes, multiple inputs and every output: only the first
    // input is used, and its original source output index survives forwarding.
    for(int output = 0; output < 8; ++output)
        expect({&reverb, output}, &oscillator, 2);
    expect({&filter, 0}, &oscillator, 2);
    oscillator.on = false;
    expect({&reverb, 0}, nullptr);
    oscillator.on = true;
    filter.on = true;
    expect({&reverb, 0}, &filter, 1);
    reverb.on = true;
    expect({&reverb, 0}, &reverb, 0);

    Node missingFirst{false, {{}, {&oscillator, 0}}};
    expect({&missingFirst, 0}, nullptr);
    Node noInput{false, {}};
    expect({&noInput, 0}, nullptr);
    expect({}, nullptr);

    // Paused feedback must terminate and resolve to silence, not recurse.
    filter.on = reverb.on = false;
    filter.inputs.front() = {&reverb, 0};
    expect({&reverb, 0}, nullptr);
    filter.inputs.front() = {&filter, 0};
    expect({&filter, 0}, nullptr);
    filter.on = true;
    expect({&reverb, 0}, &filter, 1);
    std::cout << "Audio On/Off routing tests passed\n";
}
