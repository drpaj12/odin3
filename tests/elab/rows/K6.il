# generated: elab-check --write
attribute \top 1
module \k6_func_call
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
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
  cell $add $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n70 $n69 $n68 $n67 $n66 $n65 $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 }
  end
  cell $add $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n42 $n41 $n40 $n39 }
    connect \B \b
    connect \Y { $n74 $n73 $n72 $n71 }
  end
  process $proc$K6_function_calls_function.v:9$8
    assign { $n20 $n19 $n18 $n17 } \a
    assign { $n24 $n23 $n22 $n21 } \b
    assign { $n36 $n35 $n34 $n33 } \a
    assign { $n32 $n31 $n30 $n29 } { $n42 $n41 $n40 $n39 }
    assign { $n28 $n27 $n26 $n25 } { $n74 $n73 $n72 $n71 }
    sync always
      update { $n16 $n15 $n14 $n13 } { $n28 $n27 $n26 $n25 }
  end
  connect \y { $n16 $n15 $n14 $n13 }
end
