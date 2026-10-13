# generated: elab-check --write
attribute \top 1
module \l12_metacomments
  wire width 2 input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire input 4 \c
  wire output 5 \x
  wire output 6 \y
  wire output 7 \z
  wire $n9
  wire $n10
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  process $proc$L12_metacomments.v:3$11
    assign $n9 $n10
    attribute \full_case 1
    attribute \parallel_case 1
    switch \s
      case 2'00
        assign $n10 \a
      case 2'01
        assign $n10 \b
      case 2'10
        assign $n10 \c
      case
        assign $n10 1'x
    end
    sync always
      update \x $n9
  end
  process $proc$L12_metacomments.v:9$12
    assign $n14 $n15
    attribute \parallel_case 1
    switch \s
      case 2'00
        assign $n15 \a
      case
        assign $n15 \b
    end
    sync always
      update \y $n14
  end
  process $proc$L12_metacomments.v:14$13
    assign $n16 $n17
    attribute \full_case 1
    attribute \parallel_case 1
    switch \s
      case 2'1-
        assign $n17 \a
      case 2'01
        assign $n17 \b
      case 2'00
        assign $n17 \c
      case
        assign $n17 1'x
    end
    sync always
      update \z $n16
  end
end
