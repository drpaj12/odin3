# generated: elab-check --write
attribute \top 1
module \e15_var_bit_select
  wire width 8 input 1 \a
  wire width 3 input 2 \s
  wire input 3 \d
  wire output 4 \y1
  wire output 5 \y2
  wire width 8 output 6 \r
  wire $n25
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
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
  cell $shiftx $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 3
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \s
    connect \Y \y1
  end
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 3
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \s
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
  end
  cell $shiftx $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 1
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B { $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 }
    connect \Y \y2
  end
  process $proc$E15_variable_bit_select.v:5$12
    assign { $n67 $n66 $n65 } \s
    assign { $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 } { $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 }
    switch { $n67 $n66 $n65 }
      case 3'000
        assign { $n75 $n74 $n73 $n72 $n71 $n70 $n69 } 7'0000000
        assign $n68 \d
      case 3'001
        assign { $n75 $n74 $n73 $n72 $n71 $n70 $n68 } 7'0000000
        assign $n69 \d
      case 3'010
        assign { $n75 $n74 $n73 $n72 $n71 $n69 $n68 } 7'0000000
        assign $n70 \d
      case 3'011
        assign { $n75 $n74 $n73 $n72 $n70 $n69 $n68 } 7'0000000
        assign $n71 \d
      case 3'100
        assign { $n75 $n74 $n73 $n71 $n70 $n69 $n68 } 7'0000000
        assign $n72 \d
      case 3'101
        assign { $n75 $n74 $n72 $n71 $n70 $n69 $n68 } 7'0000000
        assign $n73 \d
      case 3'110
        assign { $n75 $n73 $n72 $n71 $n70 $n69 $n68 } 7'0000000
        assign $n74 \d
      case 3'111
        assign { $n74 $n73 $n72 $n71 $n70 $n69 $n68 } 7'0000000
        assign $n75 \d
      case
        assign { $n75 $n74 $n73 $n72 $n71 $n70 $n69 $n68 } 8'00000000
    end
    sync always
      update \r { $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 }
  end
end
