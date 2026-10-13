# generated: elab-check --write
attribute \top 1
module \e6_div_mod
  wire width 8 input 1 \a
  wire width 8 input 2 \b
  wire width 8 input 3 \c
  wire width 8 output 4 \y1
  wire width 8 output 5 \y2
  wire width 8 output 6 \y3
  wire width 8 output 7 \y4
  wire width 8 output 8 \y5
  wire $n67
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
  wire $n116
  wire $n117
  wire $n118
  wire $n119
  wire $n120
  wire $n121
  wire $n122
  wire $n123
  wire $n124
  wire $n125
  wire $n126
  wire $n127
  wire $n128
  wire $n129
  wire $n130
  wire $n131
  wire $n132
  wire $n133
  wire $n134
  wire $n135
  wire $n136
  wire $n137
  wire $n138
  cell $div $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y1
  end
  cell $mod $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $div $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000011
    connect \Y { $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 }
  end
  cell $mod $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000011
    connect \Y { $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 }
  end
  cell $mod $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \b
    connect \B \c
    connect \Y { $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 }
  end
  cell $sub $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B { $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 }
    connect \Y \y5
  end
  connect \y3 { $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 }
  connect \y4 { $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 }
end
