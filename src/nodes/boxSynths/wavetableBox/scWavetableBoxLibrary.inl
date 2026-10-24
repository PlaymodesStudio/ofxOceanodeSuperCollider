void scWavetableBox::scanLibrary(){
    library.clear();libraryCategories={"All"};libraryCategory=0;
    fs::path addon=fs::path(__FILE__).parent_path();for(int i=0;i<4;++i)addon=addon.parent_path();
    const std::vector<fs::path> roots={ofToDataPath("Supercollider/Wavetables/VITAL_WT",true),addon/"data/Supercollider/Wavetables/VITAL_WT"};
    for(const auto& root:roots){if(!fs::is_directory(root))continue;libraryRoot=fs::absolute(root).string();std::error_code ec;
        for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;!ec&&it!=end;it.increment(ec)){
            if(!it->is_regular_file()||ofToLower(it->path().extension().string())!=".wav")continue;
            auto category=fs::relative(it->path().parent_path(),root).generic_string();if(category==".")category="Root";
            library.push_back({it->path().stem().string(),category,"VITAL_WT/"+fs::relative(it->path(),root).generic_string()});
            if(std::find(libraryCategories.begin(),libraryCategories.end(),category)==libraryCategories.end())libraryCategories.push_back(category);
        }break;
    }
    std::sort(library.begin(),library.end(),[](const LibraryEntry& a,const LibraryEntry& b){return a.category==b.category?a.name<b.name:a.category<b.category;});
    std::sort(libraryCategories.begin()+1,libraryCategories.end());
}
void scWavetableBox::drawLibrary(){
    if(!ImGui::CollapsingHeader("Wavetable Library",ImGuiTreeNodeFlags_DefaultOpen))return;
    ImGui::SetNextItemWidth(210);
    if(ImGui::BeginCombo("Category",libraryCategories[libraryCategory].c_str())){for(int i=0;i<int(libraryCategories.size());++i)if(ImGui::Selectable(libraryCategories[i].c_str(),i==libraryCategory))libraryCategory=i;ImGui::EndCombo();}
    ImGui::SameLine();ImGui::SetNextItemWidth(180);ImGui::InputTextWithHint("##wtsearch","Search tables",librarySearch,sizeof(librarySearch));
    ImGui::SameLine();if(ImGui::Button("Refresh"))scanLibrary();
    std::vector<int> matches;int selected=-1;
    const auto search=ofToLower(std::string(librarySearch));
    for(int i=0;i<int(library.size());++i){const auto& e=library[i];if(libraryCategory&&e.category!=libraryCategories[libraryCategory])continue;if(!search.empty()&&ofToLower(e.name+" "+e.category).find(search)==std::string::npos)continue;if(e.path==path.get())selected=int(matches.size());matches.push_back(i);}
    auto choose=[&](int i){if(matches.empty())return;const auto& e=library[matches[(i+int(matches.size()))%matches.size()]];source=0;path=e.path;};
    if(ImGui::Button("< Previous"))choose(selected<0?0:selected-1);ImGui::SameLine();if(ImGui::Button("Next >"))choose(selected+1);
    ImGui::SameLine();if(ImGui::Button("Built-in")){source=0;path="";}
    ImGui::SameLine();ImGui::TextDisabled("%d tables",int(matches.size()));
    if(ImGui::BeginCombo("Table",path.get().empty()?"Built-in":fs::path(path.get()).stem().string().c_str())){
        for(int index:matches){const auto& e=library[index];const std::string label=e.name+"  /  "+e.category;const bool chosen=e.path==path.get();if(ImGui::Selectable(label.c_str(),chosen)){source=0;path=e.path;}if(chosen)ImGui::SetItemDefaultFocus();}ImGui::EndCombo();
    }
    if(library.empty())ImGui::TextDisabled("No bundled library found in Supercollider/Wavetables/VITAL_WT.");
}
