# generated: elab-check --write
attribute \top 1
module \f3_repeat
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire width 8 output 3 \z
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  wire $n65
  wire $n66
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
  wire $n139
  wire $n140
  wire $n141
  wire $n142
  wire $n143
  wire $n144
  wire $n145
  wire $n146
  wire $n147
  wire $n148
  wire $n149
  wire $n150
  wire $n151
  wire $n152
  wire $n153
  wire $n154
  wire $n155
  wire $n156
  wire $n157
  wire $n158
  wire $n159
  wire $n160
  wire $n161
  wire $n162
  wire $n163
  wire $n164
  wire $n165
  wire $n166
  wire $n167
  wire $n168
  wire $n169
  wire $n170
  cell $shl $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 }
  end
  cell $shl $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 }
  end
  cell $shl $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
  end
  cell $shl $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 8
    connect \A { $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 }
  end
  cell $add $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n106 $n105 $n104 $n103 $n102 $n101 $n100 $n99 $n98 $n97 $n96 $n95 $n94 $n93 $n92 $n91 $n90 $n89 $n88 $n87 $n86 $n85 $n84 $n83 $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 }
  end
  cell $add $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n82 $n81 $n80 $n79 $n78 $n77 $n76 $n75 }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n138 $n137 $n136 $n135 $n134 $n133 $n132 $n131 $n130 $n129 $n128 $n127 $n126 $n125 $n124 $n123 $n122 $n121 $n120 $n119 $n118 $n117 $n116 $n115 $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 }
  end
  cell $add $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A { $n114 $n113 $n112 $n111 $n110 $n109 $n108 $n107 }
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n170 $n169 $n168 $n167 $n166 $n165 $n164 $n163 $n162 $n161 $n160 $n159 $n158 $n157 $n156 $n155 $n154 $n153 $n152 $n151 $n150 $n149 $n148 $n147 $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 }
  end
  process $proc$F3_repeat.v:4$10
    assign { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 } { $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 }
    sync always
      update \y { $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
  end
  process $proc$F3_repeat.v:10$14
    assign { $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 } { $n146 $n145 $n144 $n143 $n142 $n141 $n140 $n139 }
    sync always
      update \z { $n74 $n73 $n72 $n71 $n70 $n69 $n68 $n67 }
  end
end
