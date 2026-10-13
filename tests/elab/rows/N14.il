# generated: elab-check --write
attribute \top 1
module \n14_wired_nets
  wire input 1 \a
  wire input 2 \b
  wire output 3 \y1
  wire output 4 \y2
  wire output 5 \y3
  wire output 6 \y4
  wire \wa
  wire \wo
  wire \ta
  wire \to
  cell $and $c7
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \wa
  end
  cell $or $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \wo
  end
  cell $and $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \ta
  end
  cell $or $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \a
    connect \B \b
    connect \Y \to
  end
  connect \y1 \wa
  connect \y2 \wo
  connect \y3 \ta
  connect \y4 \to
end
