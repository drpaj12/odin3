attribute \top 1
module \t
  wire input 1 \s
  wire width 16 input 2 \d
  wire width 16 output 3 \y
  wire $n68
  wire $n69
  wire $n70
  wire $n71
  wire $n72
  wire $n73
  wire $n74
  wire $n75
  wire $n76
  wire $n77
  wire $n78
  wire $n79
  wire $n80
  wire $n81
  wire $n82
  wire $n83
  wire $n84
  wire $n85
  wire $n86
  wire $n87
  wire $n88
  wire $n89
  wire $n90
  wire $n91
  wire $n92
  wire $n93
  wire $n94
  wire $n95
  wire $n96
  wire $n97
  wire $n98
  wire $n99
  wire $n100
  wire $n101
  wire $n102
  wire $n103
  wire $n104
  wire $n105
  wire $n106
  wire $n107
  wire $n108
  wire $n109
  wire $n110
  wire $n111
  wire $n112
  wire $n113
  wire $n114
  wire $n115
  cell $mux $c8
    parameter \WIDTH 16
    connect \A { $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 }
    connect \B { $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 }
    connect \S \s
    connect \Y \y
  end
  cell $neg $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A \d
    connect \Y { $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 }
  end
  cell $mux $c10
    parameter \WIDTH 16
    connect \A { $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 }
    connect \B \d
    connect \S \s
    connect \Y { $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 }
  end
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 16
    parameter \B_WIDTH 16
    parameter \Y_WIDTH 16
    connect \A { $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 }
    connect \B 16'0000000000000001
    connect \Y { $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 }
  end
end
