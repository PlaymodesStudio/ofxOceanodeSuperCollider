//
//  scChannelRouterMatrix.cpp
//  ofxOceanodeSupercollider
//
//  Channel Router Matrix - Route inputs to outputs with gain control
//

#include "ofxOceanodeSuperColliderConfig.h"
#include "scChannelRouterMatrix.h"
#include "ofxSCSynth.h"
#include "ofxSuperCollider.h"
#include "imgui.h"

scChannelRouterMatrix::scChannelRouterMatrix() : scNode("Channel Router Matrix") {
	hoveredCell[0] = -1;
	hoveredCell[1] = -1;
	isDragging = false;
}

scChannelRouterMatrix::~scChannelRouterMatrix() {
	try {
		listeners.unsubscribeAll();
		
		// Free all synth instances
		for(auto& pair : synthInstances) {
			if(pair.second != nullptr) {
				pair.second->free();
				delete pair.second;
			}
		}
		synthInstances.clear();
		
		inputBuses.clear();
		outputBuses.clear();
		
	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error in destructor: " << e.what();
	}
}

void scChannelRouterMatrix::setup() {
	ofLogNotice("scChannelRouterMatrix") << "Setup called";
	
	try {
		// Core parameters
		addParameter(numChannels.set("Num Channels", 2, 1, MAX_NODE_CHANNELS));
		
		// Bypass parameter
		addParameter(bypass.set("Bypass", false));
		
		// Mode dropdown: Multislider (0) or Toggle (1)
		vector<string> modeOptions = {"Multislider", "Toggle"};
		addParameterDropdown(matrixMode, "Mode", 0, modeOptions);
		
		// Compensation dropdown: 0dB, -3dB, -6dB
		vector<string> compensationOptions = {"0dB", "-3dB", "-6dB"};
		addParameterDropdown(compensation, "Compensation", 1, compensationOptions); // Default -3dB
		
		// Inspector parameters for widget dimensions
		addInspectorParameter(widgetWidth.set("Widget Width", 240.0f, 100.0f, 1200.0f));
		addInspectorParameter(widgetHeight.set("Widget Height", 240.0f, 100.0f, 1200.0f));
		
		// Add single input and output
		scNode::addInput("In");
		scNode::addOutput("Out");
		
		// Initialize matrix
		initializeMatrix();
		
		// Add matrix widget
		addCustomRegion(
			ofParameter<std::function<void()>>().set("Matrix", [this](){
				drawMatrixWidget();
			}),
			ofParameter<std::function<void()>>().set("Matrix_Region", [this](){
				drawMatrixWidget();
			})
		);
		
		// Set up parameter listeners
		listeners.push(numChannels.newListener([this](int &n){
			updateMatrixSize();
			// Trigger graph recomputation
			for(auto& output : outputs) {
				output = output;
			}
		}));
		
		listeners.push(bypass.newListener([this](bool &b){
			// Update synth parameters
			for(auto& pair : synthInstances) {
				if(pair.first != nullptr) {
					updateSynthParameters(pair.first);
				}
			}
		}));
		
		listeners.push(matrixMode.newListener([this](int &mode){
			// When switching modes, reset matrix to identity
			int n = numChannels.get();
			for(int i = 0; i < n; i++) {
				for(int j = 0; j < n; j++) {
					routingMatrix[i][j] = (i == j) ? 1.0f : 0.0f;
				}
			}
			
			// Update synth parameters
			for(auto& pair : synthInstances) {
				if(pair.first != nullptr) {
					updateSynthParameters(pair.first);
				}
			}
		}));
		
		listeners.push(compensation.newListener([this](int &comp){
			// Update synth parameters
			for(auto& pair : synthInstances) {
				if(pair.first != nullptr) {
					updateSynthParameters(pair.first);
				}
			}
		}));
		
	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error in setup(): " << e.what();
		throw;
	}
}

string scChannelRouterMatrix::getSynthDefName() const {
	return "channelRouterMatrix" + ofToString(numChannels.get());
}

void scChannelRouterMatrix::initializeMatrix() {
	int n = numChannels.get();
	routingMatrix.resize(n);
	for(int i = 0; i < n; i++) {
		routingMatrix[i].resize(n, 0.0f);
		routingMatrix[i][i] = 1.0f; // Identity matrix
	}
	ofLogNotice("scChannelRouterMatrix") << "Initialized " << n << "x" << n << " identity matrix";
}

void scChannelRouterMatrix::updateMatrixSize() {
	int n = numChannels.get();
    if(n < 1 || n > MAX_NODE_CHANNELS) return;
	int oldSize = routingMatrix.size();
	
	vector<vector<float>> oldMatrix = routingMatrix;
	
	routingMatrix.resize(n);
	for(int i = 0; i < n; i++) {
		routingMatrix[i].resize(n, 0.0f);
		
		// Copy old values if they exist
		if(i < oldSize) {
			for(int j = 0; j < n && j < oldSize; j++) {
				routingMatrix[i][j] = oldMatrix[i][j];
			}
		}
		
		// Set identity for new rows/columns
		if(i >= oldSize) {
			routingMatrix[i][i] = 1.0f;
		}
	}
	
	ofLogNotice("scChannelRouterMatrix") << "Resized matrix to " << n << "x" << n;
}

vector<float> scChannelRouterMatrix::flattenMatrix() const {
	vector<float> flattened;
	int n = numChannels.get();
	flattened.reserve(n * n);
	
	for(int i = 0; i < n; i++) {
		for(int j = 0; j < n; j++) {
			flattened.push_back(routingMatrix[i][j]);
		}
	}
	
	return flattened;
}

void scChannelRouterMatrix::activate() {
	for(auto& pair : synthInstances) if(pair.second) pair.second->run(true);
}

void scChannelRouterMatrix::deactivate() {
	for(auto& pair : synthInstances) if(pair.second) pair.second->run(false);
}

void scChannelRouterMatrix::buildSynth(ofxSCServer* server) {
	ofLogNotice("scChannelRouterMatrix") << "Building synth for server";
}

void scChannelRouterMatrix::createSynth(ofxSCServer* server) {
	ofLogNotice("scChannelRouterMatrix") << "Creating synth";
	
	if(server == nullptr) {
		ofLogError("scChannelRouterMatrix") << "Server is NULL!";
		return;
	}
	
	try {
		if(synthInstances[server] != nullptr) {
			synthInstances[server]->free();
			delete synthInstances[server];
		}
		
		synthInstances[server] = new ofxSCSynth(getSynthDefName(), server);
		synthInstances[server]->create();
		synthInstances[server]->run(getActive());

		// Set bypass parameter immediately
		synthInstances[server]->set("bypass", bypass.get() ? 1.0f : 0.0f);
		
		updateSynthParameters(server);
		
		if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
			for(auto& pair : inputBuses[server]) {
				synthInstances[server]->set("in", pair.second);
				break;
			}
		}
		
		if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
			synthInstances[server]->set("out", outputBuses[server][0]);
		}
		
		ofLogNotice("scChannelRouterMatrix") << "Synth created successfully";
		
	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error creating synth: " << e.what();
	}
}

void scChannelRouterMatrix::updateSynthParameters(ofxSCServer* server) {
	if(server == nullptr || synthInstances.count(server) == 0 || synthInstances[server] == nullptr) {
		return;
	}
	
	try {
		vector<float> matrixData = flattenMatrix();
		synthInstances[server]->set("matrix", matrixData);
		synthInstances[server]->set("compensation", (float)compensation.get());
		synthInstances[server]->set("bypass", bypass.get() ? 1.0f : 0.0f);
		
		ofLogVerbose("scChannelRouterMatrix") << "Updated synth parameters";
		
	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error updating synth parameters: " << e.what();
	}
}

void scChannelRouterMatrix::free(ofxSCServer* server) {
	if(server == nullptr) return;
	
	try {
		if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
			synthInstances[server]->free();
			delete synthInstances[server];
			synthInstances.erase(server);
		}
		
		inputBuses.erase(server);
		outputBuses.erase(server);
		
	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error in free(): " << e.what();
	}
}

void scChannelRouterMatrix::setInputBus(ofxSCServer* server, scNode* node, int bus) {
	if(server == nullptr || node == nullptr) return;
	
	inputBuses[server][node] = bus;
	ofLogNotice("scChannelRouterMatrix") << "Input bus set to " << bus;
	
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			synthInstances[server]->set("in", bus);
		} catch(const std::exception& e) {
			ofLogError("scChannelRouterMatrix") << "Error setting input bus: " << e.what();
		}
	}
}

void scChannelRouterMatrix::resetInputBusses(ofxSCServer* server, int targetBus) {
	inputBuses[server].clear();
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		synthInstances[server]->set("in", targetBus);
	}
}

void scChannelRouterMatrix::setOutputBus(ofxSCServer* server, int index, int bus) {
	if(server == nullptr) return;
	
	outputBuses[server][index] = bus;
	ofLogNotice("scChannelRouterMatrix") << "Output bus set to " << bus;
	
	if(synthInstances.count(server) > 0 && synthInstances[server] != nullptr) {
		try {
			synthInstances[server]->set("out", bus);
		} catch(const std::exception& e) {
			ofLogError("scChannelRouterMatrix") << "Error setting output bus: " << e.what();
		}
	}
}

int scChannelRouterMatrix::getOutputBusIndex(ofxSCServer* server, int index) {
	if(outputBuses.count(server) > 0 && outputBuses[server].count(index) > 0) {
		return outputBuses[server][index];
	}
	return -1;
}

void scChannelRouterMatrix::moveSynthBefore(ofxSCServer* server, int nodeID) {
	if(server == nullptr) return;

	auto it = synthInstances.find(server);
	if(it == synthInstances.end() || it->second == nullptr) {
		return;
	}

	ofxSCSynth* synth = it->second;

	try {
		// Ensure matrix + compensation + bypass are up to date
		updateSynthParameters(server);

		// Restore input bus (first mapped input, if any)
		if(inputBuses.count(server) > 0 && !inputBuses[server].empty()) {
			int inBus = inputBuses[server].begin()->second;
			synth->set("in", inBus);
		}

		// Restore output bus 0, if present
		if(outputBuses.count(server) > 0 && outputBuses[server].count(0) > 0) {
			synth->set("out", outputBuses[server][0]);
		}

		// Move synth inside SC graph
		synth->moveBefore(nodeID);

	} catch(const std::exception& e) {
		ofLogError("scChannelRouterMatrix") << "Error in moveSynthBefore(): " << e.what();
	}
}

int scChannelRouterMatrix::getLastSynthID(ofxSCServer* server) {
	if(server == nullptr) return -1;

	auto it = synthInstances.find(server);
	if(it != synthInstances.end() && it->second != nullptr) {
		return it->second->nodeID;
	}
	return -1;
}

void scChannelRouterMatrix::presetSave(ofJson &json) {
	int n = numChannels.get();
	for(int i = 0; i < n; i++) {
		for(int j = 0; j < n; j++) {
			json["Matrix"][i][j] = routingMatrix[i][j];
		}
	}
}

void scChannelRouterMatrix::presetRecallAfterSettingParameters(ofJson &json) {
	if(json.count("Matrix") == 1) {
		try {
			int n = numChannels.get();
			for(int i = 0; i < n && i < json["Matrix"].size(); i++) {
				for(int j = 0; j < n && j < json["Matrix"][i].size(); j++) {
					routingMatrix[i][j] = json["Matrix"][i][j];
				}
			}
			
			for(auto& pair : synthInstances) {
				if(pair.first != nullptr) {
					updateSynthParameters(pair.first);
				}
			}
			
		} catch(const std::exception& e) {
			ofLogError("scChannelRouterMatrix") << "Error loading preset: " << e.what();
		}
	}
}

void scChannelRouterMatrix::drawMatrixWidget() {
	if(matrixMode.get() == 0) {
		drawMultisliderMatrix();
	} else {
		drawToggleMatrix();
	}
}

void scChannelRouterMatrix::drawMultisliderMatrix() {
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	int n = numChannels.get();
	float maxWidth = widgetWidth.get();
	float maxHeight = widgetHeight.get();
	
	float labelWidth = 25.0f;
	float labelHeight = 20.0f;
	float availableWidth = maxWidth - labelWidth;
	float availableHeight = maxHeight - labelHeight;
	
	float cellWidth = availableWidth / n;
	float cellHeight = availableHeight / n;
	
	float matrixWidth = cellWidth * n;
	float matrixHeight = cellHeight * n;
	
	// Column labels
	for(int j = 0; j < n; j++) {
		char label[8];
		sprintf(label, "O%d", j + 1);
		ImVec2 labelPos = ImVec2(
			cursorPos.x + labelWidth + j * cellWidth + cellWidth * 0.5f - 7,
			cursorPos.y + 2
		);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), label);
	}
	
	ImVec2 matrixStart = ImVec2(cursorPos.x + labelWidth, cursorPos.y + labelHeight);
	
	// Background
	drawList->AddRectFilled(
		matrixStart,
		ImVec2(matrixStart.x + matrixWidth, matrixStart.y + matrixHeight),
		IM_COL32(20, 20, 20, 255)
	);
	
	// Track which row we're currently dragging in
	static int dragRow = -1;
	int currentRow = -1;
	int currentCol = -1;
	
	// Create one invisible button per row for row-based interaction
	for(int i = 0; i < n; i++) {
		ImVec2 rowStart = ImVec2(matrixStart.x, matrixStart.y + i * cellHeight);
		ImVec2 rowSize = ImVec2(matrixWidth, cellHeight);
		
		ImGui::SetCursorScreenPos(rowStart);
		ImGui::PushID(i);
		ImGui::InvisibleButton("##RowInteraction", rowSize);
		bool isRowActive = ImGui::IsItemActive();
		bool isRowHovered = ImGui::IsItemHovered();
		ImGui::PopID();
		
		ImVec2 mousePos = ImGui::GetIO().MousePos;
		
		// Handle right-click to toggle cell between 0 and 1
		if(isRowHovered && ImGui::IsMouseClicked(1)) {
			float relX = mousePos.x - matrixStart.x;
			
			if(relX >= 0 && relX < matrixWidth) {
				int clickedCol = (int)(relX / cellWidth);
				
				if(clickedCol >= 0 && clickedCol < n) {
					// Toggle between 0 and 1
					routingMatrix[i][clickedCol] = (routingMatrix[i][clickedCol] > 0.5f) ? 0.0f : 1.0f;
					
					for(auto& pair : synthInstances) {
						if(pair.first != nullptr) {
							updateSynthParameters(pair.first);
						}
					}
				}
			}
		}
		
		// Handle left-click drag within this row only
		if((isRowHovered || isRowActive) && ImGui::IsMouseDown(0)) {
			float relX = mousePos.x - matrixStart.x;
			float relY = mousePos.y - rowStart.y;
			
			if(relX >= 0 && relX < matrixWidth && relY >= 0 && relY < cellHeight) {
				currentCol = (int)(relX / cellWidth);
				currentRow = i;
				
				// On first click in this row, remember which row we're dragging
				if(ImGui::IsMouseClicked(0)) {
					dragRow = i;
				}
				
				// Only allow dragging within the same row where we started
				if(dragRow == i && currentCol >= 0 && currentCol < n) {
					float normalizedY = 1.0f - ofClamp(relY / cellHeight, 0.0f, 1.0f);
					
					if(ImGui::GetIO().KeyShift) {
						normalizedY = std::round(normalizedY * 10.0f) / 10.0f;
					}
					
					routingMatrix[i][currentCol] = normalizedY;
					
					for(auto& pair : synthInstances) {
						if(pair.first != nullptr) {
							updateSynthParameters(pair.first);
						}
					}
				}
			}
		}
	}
	
	// Reset drag row when mouse is released
	if(ImGui::IsMouseReleased(0)) {
		dragRow = -1;
	}
	
	// Draw cells
	for(int i = 0; i < n; i++) {
		char label[8];
		sprintf(label, "I%d", i + 1);
		ImVec2 labelPos = ImVec2(
			cursorPos.x + 2,
			matrixStart.y + i * cellHeight + cellHeight * 0.5f - 7
		);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), label);
		
		for(int j = 0; j < n; j++) {
			ImVec2 cellStart;
			getCellPosition(i, j, matrixStart, cellWidth, cellHeight, cellStart);
			ImVec2 cellEnd = ImVec2(cellStart.x + cellWidth - 1, cellStart.y + cellHeight - 1);
			
			bool isCellHovered = (i == currentRow && j == currentCol);
			
			float value = routingMatrix[i][j];
			float barHeight = (cellHeight - 1) * value;
			
			ImVec2 barStart = ImVec2(cellStart.x, cellEnd.y - barHeight);
			
			drawList->AddRectFilled(cellStart, cellEnd, IM_COL32(30, 30, 30, 255));
			
			if(value > 0.01f) {
				ImU32 barColor;
				
				if(value < 0.5f) {
					int blue = (int)(100 + 155 * (value / 0.5f));
					int green = (int)(100 * (value / 0.5f));
					barColor = IM_COL32(0, green, blue, 255);
				} else {
					float t = (value - 0.5f) / 0.5f;
					int red = (int)(200 * t);
					int green = 200;
					int blue = (int)(255 * (1.0f - t));
					barColor = IM_COL32(red, green, blue, 255);
				}
				
				if(isCellHovered) {
					ImU32 r = (barColor >> 0) & 0xFF;
					ImU32 g = (barColor >> 8) & 0xFF;
					ImU32 b = (barColor >> 16) & 0xFF;
					r = std::min(r + 50, 255u);
					g = std::min(g + 50, 255u);
					b = std::min(b + 50, 255u);
					barColor = IM_COL32(r, g, b, 255);
				}
				
				drawList->AddRectFilled(barStart, cellEnd, barColor);
			}
		}
	}
	
	// Grid
	drawList->AddRect(matrixStart, ImVec2(matrixStart.x + matrixWidth, matrixStart.y + matrixHeight),
		IM_COL32(80, 80, 80, 255), 0.0f, 0, 1.0f);
	
	for(int i = 1; i < n; i++) {
		float x = matrixStart.x + i * cellWidth;
		drawList->AddLine(ImVec2(x, matrixStart.y), ImVec2(x, matrixStart.y + matrixHeight),
			IM_COL32(60, 60, 60, 128));
		
		float y = matrixStart.y + i * cellHeight;
		drawList->AddLine(ImVec2(matrixStart.x, y), ImVec2(matrixStart.x + matrixWidth, y),
			IM_COL32(60, 60, 60, 128));
	}
	
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, matrixStart.y + matrixHeight + 5));
	ImGui::Dummy(ImVec2(maxWidth, 2.0f));
}

void scChannelRouterMatrix::drawToggleMatrix() {
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	ImVec2 cursorPos = ImGui::GetCursorScreenPos();
	
	int n = numChannels.get();
	float maxWidth = widgetWidth.get();
	float maxHeight = widgetHeight.get();
	
	float labelWidth = 25.0f;
	float labelHeight = 20.0f;
	float availableWidth = maxWidth - labelWidth;
	float availableHeight = maxHeight - labelHeight;
	
	float cellWidth = availableWidth / n;
	float cellHeight = availableHeight / n;
	
	float matrixWidth = cellWidth * n;
	float matrixHeight = cellHeight * n;
	
	// Column labels
	for(int j = 0; j < n; j++) {
		char label[8];
		sprintf(label, "O%d", j + 1);
		ImVec2 labelPos = ImVec2(
			cursorPos.x + labelWidth + j * cellWidth + cellWidth * 0.5f - 7,
			cursorPos.y + 2
		);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), label);
	}
	
	ImVec2 matrixStart = ImVec2(cursorPos.x + labelWidth, cursorPos.y + labelHeight);
	
	// Background
	drawList->AddRectFilled(
		matrixStart,
		ImVec2(matrixStart.x + matrixWidth, matrixStart.y + matrixHeight),
		IM_COL32(20, 20, 20, 255)
	);
	
	// Interaction
	ImGui::SetCursorScreenPos(matrixStart);
	ImGui::InvisibleButton("##ToggleMatrixInteraction", ImVec2(matrixWidth, matrixHeight));
	bool isHovered = ImGui::IsItemHovered();
	bool isActive = ImGui::IsItemActive();
	
	ImVec2 mousePos = ImGui::GetIO().MousePos;
	int currentRow = -1;
	int currentCol = -1;
	
	static bool dragValue = false;
	static int lastPaintedRow = -1;
	static int lastPaintedCol = -1;
	
	if((isHovered || isActive) && ImGui::IsMouseDown(0)) {
		float relX = mousePos.x - matrixStart.x;
		float relY = mousePos.y - matrixStart.y;
		
		if(relX >= 0 && relX < matrixWidth && relY >= 0 && relY < matrixHeight) {
			currentCol = (int)(relX / cellWidth);
			currentRow = (int)(relY / cellHeight);
			
			if(currentCol >= 0 && currentCol < n && currentRow >= 0 && currentRow < n) {
				if(ImGui::IsMouseClicked(0)) {
					dragValue = (routingMatrix[currentRow][currentCol] > 0.5f) ? false : true;
					routingMatrix[currentRow][currentCol] = dragValue ? 1.0f : 0.0f;
					lastPaintedRow = currentRow;
					lastPaintedCol = currentCol;
					
					for(auto& pair : synthInstances) {
						if(pair.first != nullptr) {
							updateSynthParameters(pair.first);
						}
					}
				}
				else if(isActive && (currentRow != lastPaintedRow || currentCol != lastPaintedCol)) {
					routingMatrix[currentRow][currentCol] = dragValue ? 1.0f : 0.0f;
					lastPaintedRow = currentRow;
					lastPaintedCol = currentCol;
					
					for(auto& pair : synthInstances) {
						if(pair.first != nullptr) {
							updateSynthParameters(pair.first);
						}
					}
				}
			}
		}
	}
	
	if(ImGui::IsMouseReleased(0)) {
		lastPaintedRow = -1;
		lastPaintedCol = -1;
	}
	
	// Draw cells
	for(int i = 0; i < n; i++) {
		char label[8];
		sprintf(label, "I%d", i + 1);
		ImVec2 labelPos = ImVec2(
			cursorPos.x + 2,
			matrixStart.y + i * cellHeight + cellHeight * 0.5f - 7
		);
		drawList->AddText(labelPos, IM_COL32(180, 180, 180, 255), label);
		
		for(int j = 0; j < n; j++) {
			ImVec2 cellStart;
			getCellPosition(i, j, matrixStart, cellWidth, cellHeight, cellStart);
			ImVec2 cellEnd = ImVec2(cellStart.x + cellWidth - 1, cellStart.y + cellHeight - 1);
			
			bool isActiveCell = (routingMatrix[i][j] > 0.5f);
			bool isCellHovered = (i == currentRow && j == currentCol);
			
			ImU32 cellColor = isActiveCell ? IM_COL32(220, 220, 220, 255) : IM_COL32(30, 30, 30, 255);
			
			if(isCellHovered) {
				cellColor = isActiveCell ? IM_COL32(255, 255, 255, 255) : IM_COL32(60, 60, 60, 255);
			}
			
			drawList->AddRectFilled(cellStart, cellEnd, cellColor);
		}
	}
	
	// Grid
	drawList->AddRect(matrixStart, ImVec2(matrixStart.x + matrixWidth, matrixStart.y + matrixHeight),
		IM_COL32(80, 80, 80, 255), 0.0f, 0, 1.0f);
	
	for(int i = 1; i < n; i++) {
		float x = matrixStart.x + i * cellWidth;
		drawList->AddLine(ImVec2(x, matrixStart.y), ImVec2(x, matrixStart.y + matrixHeight),
			IM_COL32(60, 60, 60, 128));
		
		float y = matrixStart.y + i * cellHeight;
		drawList->AddLine(ImVec2(matrixStart.x, y), ImVec2(matrixStart.x + matrixWidth, y),
			IM_COL32(60, 60, 60, 128));
	}
	
	ImGui::SetCursorScreenPos(ImVec2(cursorPos.x, matrixStart.y + matrixHeight + 5));
	ImGui::Dummy(ImVec2(maxWidth, 2.0f));
}

void scChannelRouterMatrix::getCellPosition(int row, int col, const ImVec2& matrixStart, float cellWidth, float cellHeight, ImVec2& result) {
	result.x = matrixStart.x + col * cellWidth;
	result.y = matrixStart.y + row * cellHeight;
}

void scChannelRouterMatrix::drawSeparator() {
	ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(
		ImVec2(p.x, p.y),
		ImVec2(p.x + 240, p.y),
		IM_COL32(200, 200, 200, 255),
		1.0f
	);
	ImGui::Dummy(ImVec2(0, 4));
}
