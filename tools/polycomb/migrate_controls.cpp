#include "scPolyCombMigration.h"
#include <iostream>
int main(){
    pcmigration::Json input; std::cin>>input;
    for(auto& c:input) c=pcmigration::controls(c);
    std::cout<<input.dump();
}
