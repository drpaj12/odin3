# generated: elab-check --write
attribute \top 1
module \c10_full_parallel
  wire width 2 input 1 \s
  wire input 2 \a
  wire input 3 \b
  wire input 4 \c
  wire output 5 \w
  wire output 6 \x
  wire output 7 \y
  wire output 8 \z
  wire $n10
  wire $n11
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  process $proc$C10_full_parallel_case.v:3$12
    assign $n10 $n11
    attribute \full_case 1
    attribute \parallel_case 1
    switch \s
      case 2'00
        assign $n11 \a
      case 2'01
        assign $n11 \b
      case 2'10
        assign $n11 \c
      case
        assign $n11 1'x
    end
    sync always
      update \w $n10
  end
  process $proc$C10_full_parallel_case.v:10$13
    assign $n15 $n16
    attribute \parallel_case 1
    switch \s
      case 2'1-
        assign $n16 \a
      case 2'01
        assign $n16 \b
      case
        assign $n16 \c
    end
    sync always
      update \x $n15
  end
  process $proc$C10_full_parallel_case.v:17$14
    assign $n17 $n18
    attribute \full_case 1
    attribute \parallel_case 1
    switch \s
      case 2'00
        assign $n18 \a
      case 2'01
        assign $n18 \b
      case
        assign $n18 1'x
    end
    sync always
      update \y $n17
  end
  process $proc$C10_full_parallel_case.v:22$15
    assign $n19 $n20
    attribute \full_case 1
    attribute \parallel_case 1
    switch \s
      case 2'00
        assign $n20 \a
      case 2'11
        assign $n20 \b
      case
        assign $n20 1'x
    end
    sync always
      update \z $n19
  end
end
