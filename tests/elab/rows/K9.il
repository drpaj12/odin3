# generated: elab-check --write
attribute \top 1
module \k9_tasks
  wire input 1 \clk
  wire width 4 input 2 \a
  wire width 4 input 3 \b
  wire width 4 output 4 \s
  wire width 4 output 5 \d
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
  cell $add $c6
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n41 $n40 $n39 $n38 }
  end
  cell $sub $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n57 $n56 $n55 $n54 }
  end
  process $proc$K9_tasks.v:14$8
    assign { $n29 $n28 $n27 $n26 } \a
    assign { $n33 $n32 $n31 $n30 } \b
    assign { $n37 $n36 $n35 $n34 } { $n41 $n40 $n39 $n38 }
    assign { $n21 $n20 $n19 $n18 } { $n41 $n40 $n39 $n38 }
    assign { $n45 $n44 $n43 $n42 } \a
    assign { $n49 $n48 $n47 $n46 } \b
    assign { $n53 $n52 $n51 $n50 } { $n57 $n56 $n55 $n54 }
    assign { $n25 $n24 $n23 $n22 } { $n57 $n56 $n55 $n54 }
    sync posedge \clk
      update \s { $n21 $n20 $n19 $n18 }
      update \d { $n25 $n24 $n23 $n22 }
  end
end
