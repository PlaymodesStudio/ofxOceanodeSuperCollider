
FluteStk : UGen {

    *ar{|basicparameters(0), envelopesandvibrato(0), physicalandnonlinearity(0)|
      ^this.multiNew('audio', basicparameters, envelopesandvibrato, physicalandnonlinearity)
    }

    *kr{|basicparameters(0), envelopesandvibrato(0), physicalandnonlinearity(0)|
      ^this.multiNew('control', basicparameters, envelopesandvibrato, physicalandnonlinearity)
    }

    name { ^"FluteStk" }

    info { ^"Generated with Faust" }
    
    
}
