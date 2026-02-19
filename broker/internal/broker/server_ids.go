package broker

import "regexp"

var agentIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)
var deploymentIDRe = regexp.MustCompile(`^[A-Za-z0-9_.-]{1,128}$`)
var teamIDRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)
